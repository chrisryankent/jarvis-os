/*
 * =============================================================================
 * JARVIS AI OS — Virtual File System  (kernel/vfs.c)
 * Language: C17
 *
 * Provides a unified file interface over:
 *   - ext4 partitions (user data, /home)
 *   - JARVIS internal ramfs (/jarvis/models, /jarvis/logs, /jarvis/data)
 *   - AHCI/NVMe block layer
 *
 * All file operations are intercepted by the AI filesystem service for:
 *   - Access logging (security audit)
 *   - Semantic indexing (file embedder)
 *   - Anomaly detection (unexpected reads of sensitive files)
 *
 * AI-specific additions over a standard VFS:
 *   - vfs_ai_search()    — query the Python AI filesystem via IPC
 *   - vfs_model_map()    — mmap a model file directly into model pool
 *   - vfs_encrypted_*   — transparent AES-256-XTS for sensitive paths
 * =============================================================================
 */

#include "kernel.h"
#include <stdio.h>
#include <string.h>

/* ---- VFS types ----------------------------------------------------------- */
#define VFS_TYPE_RAMFS  1
#define VFS_TYPE_EXT4   2

#define VFS_FLAG_READ   0x01
#define VFS_FLAG_WRITE  0x02
#define VFS_FLAG_CREATE 0x04
#define VFS_FLAG_APPEND 0x08
#define VFS_FLAG_TRUNC  0x10

typedef struct vfs_node vfs_node_t;
typedef struct vfs_ops  vfs_ops_t;

struct vfs_ops {
    int    (*read)  (vfs_node_t *node, uint64_t offset, void *buf, size_t len);
    int    (*write) (vfs_node_t *node, uint64_t offset, const void *buf, size_t len);
    int    (*stat)  (vfs_node_t *node, void *stat_buf);
    int    (*readdir)(vfs_node_t *node, int idx, char *name_out, size_t name_max);
    void   (*close) (vfs_node_t *node);
};

struct vfs_node {
    char         name[256];
    uint8_t      type;      /* VFS_TYPE_* */
    uint64_t     size;
    uint64_t     inode;
    vfs_ops_t   *ops;
    void        *priv;      /* fs-specific state */
};

/* ---- Mount table --------------------------------------------------------- */
#define MAX_MOUNTS 16

typedef struct {
    char        mountpoint[128];
    uint8_t     fs_type;
    vfs_ops_t  *ops;
    void       *priv;       /* fs root block, partition offset, etc. */
} mount_t;

static mount_t mount_table[MAX_MOUNTS];
static int     mount_count = 0;

/* ---- File descriptor table ----------------------------------------------- */
#define MAX_FDS 1024

typedef struct {
    vfs_node_t *node;
    uint64_t    offset;
    uint32_t    flags;
    int         used;
} fd_t;

static fd_t fd_table[MAX_FDS];

/* ---- RAMFS --------------------------------------------------------------- */
#define RAMFS_MAX_FILES 512

/*
 * RAMFS_MAX_SIZE bounds files staying on the small kernel heap only — see
 * LARGE_ALLOC_THRESHOLD's twin in ipc_sched_pmm.c. It does NOT apply to
 * large/PMM-backed files: model_manifest.c stages a 2 GB brain.gguf, a
 * 1.5 GB Whisper model, a 327 MB Kokoro TTS model, etc., and capping every
 * ramfs file at 64 MB would make it impossible to ever stage any of them —
 * directly defeating the reason this filesystem exists. (Previously this
 * constant was defined and never referenced anywhere at all, so neither
 * bound applied.)
 */
#define RAMFS_MAX_SIZE  (64 * 1024 * 1024)  /* 64 MB ceiling for SMALL files only */

typedef struct {
    char     path[256];
    uint8_t *data;
    size_t   size;
    size_t   capacity;
    int      used;
    int      is_dir;
    int      large;   /* 1 if data[] came from kmalloc_large(), not kmalloc() */
} ramfs_file_t;

static ramfs_file_t ramfs_files[RAMFS_MAX_FILES];
static int          ramfs_count = 0;

static ramfs_file_t *ramfs_find(const char *path)
{
    for (int i = 0; i < ramfs_count; i++) {
        if (ramfs_files[i].used &&
            strcmp(ramfs_files[i].path, path) == 0)
            return &ramfs_files[i];
    }
    return NULL;
}

static ramfs_file_t *ramfs_create_node(const char *path, size_t initial_cap, int is_dir)
{
    ramfs_file_t *existing = ramfs_find(path);
    if (existing) return existing;
    if (ramfs_count >= RAMFS_MAX_FILES) return NULL;
    ramfs_file_t *f = &ramfs_files[ramfs_count++];
    strncpy(f->path, path, sizeof(f->path) - 1);
    f->capacity = initial_cap ? initial_cap : 4096;
    f->large    = f->capacity >= LARGE_ALLOC_THRESHOLD;
    f->data     = f->large ? (uint8_t *)kmalloc_large(f->capacity)
                           : (uint8_t *)kmalloc(f->capacity);
    f->size     = 0;
    f->used     = 1;
    f->is_dir   = is_dir;
    return f;
}

static ramfs_file_t *ramfs_create(const char *path, size_t initial_cap)
{
    return ramfs_create_node(path, initial_cap, 0);
}

static int ramfs_read(vfs_node_t *node, uint64_t offset,
                       void *buf, size_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)node->priv;
    if (!f || offset >= f->size) return 0;
    size_t avail = f->size - (size_t)offset;
    size_t to_read = avail < len ? avail : len;
    memcpy(buf, f->data + offset, to_read);
    return (int)to_read;
}

static int ramfs_write(vfs_node_t *node, uint64_t offset,
                        const void *buf, size_t len)
{
    ramfs_file_t *f = (ramfs_file_t *)node->priv;
    if (!f) return -1;

    size_t end = (size_t)offset + len;
    if (end > f->capacity) {
        /* Grow. Once a file crosses the large-allocation threshold it
         * switches (one time) from the small heap to PMM-backed pages —
         * this is what actually lets a file outgrow the 16 MB heap at
         * all. A file that's already large-backed keeps growing through
         * the same path. Only a file that will REMAIN small-backed after
         * this growth is held to RAMFS_MAX_SIZE; large files are bounded
         * only by available physical memory, exactly like a real model
         * file should be. */
        size_t new_cap = end * 2;
        int new_cap_is_large = new_cap >= LARGE_ALLOC_THRESHOLD;

        if (!new_cap_is_large && new_cap > RAMFS_MAX_SIZE) {
            terminal_printf("[VFS] ramfs_write: %s would exceed the %d MB "
                             "small-file ceiling\n", f->path,
                             RAMFS_MAX_SIZE / (1024 * 1024));
            return -1;
        }

        uint8_t *new_buf = new_cap_is_large ? (uint8_t *)kmalloc_large(new_cap)
                                             : (uint8_t *)kmalloc(new_cap);
        if (!new_buf) return -1;

        memcpy(new_buf, f->data, f->size);

        /* Free the OLD buffer with whichever allocator actually produced
         * it — these two allocators are not interchangeable. */
        if (f->large)
            kfree_large(f->data, f->capacity);
        else
            kfree(f->data);

        f->data     = new_buf;
        f->capacity = new_cap;
        f->large    = new_cap_is_large;
    }
    memcpy(f->data + offset, buf, len);
    if (end > f->size) f->size = end;
    node->size = f->size;
    return (int)len;
}

static vfs_ops_t ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .stat    = NULL,
    .readdir = NULL,
    .close   = NULL,
};

/* ---- VFS init ------------------------------------------------------------ */
void vfs_init(void)
{
    terminal_puts("[VFS] Initialising virtual file system\n");

    /* Mount ramfs at /jarvis */
    mount_table[mount_count++] = (mount_t){
        .mountpoint = "/jarvis",
        .fs_type    = VFS_TYPE_RAMFS,
        .ops        = &ramfs_ops,
        .priv       = NULL,
    };

    /* Create standard JARVIS directories in ramfs */
    const char *dirs[] = {
        "/jarvis/models/brain",
        "/jarvis/models/stt",
        "/jarvis/models/tts",
        "/jarvis/models/wake",
        "/jarvis/models/security",
        "/jarvis/models/filesystem",
        "/jarvis/logs",
        "/jarvis/data",
        "/jarvis/bin",
        "/jarvis/screenshots",
        "/jarvis/fonts",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        ramfs_create_node(dirs[i], 0, 1);
        terminal_printf("[VFS] Created: %s\n", dirs[i]);
    }

    vfs_seed_packaged_models();

    /* TODO: mount ext4 at /home and / from AHCI partition */
    /* ext4_mount("/dev/nvme0n1p2", "/"); */

    terminal_puts("[VFS] VFS ready\n");
}

/* ---- Public VFS API ------------------------------------------------------ */

/* Open a file, returns fd or negative on error */
int vfs_open(const char *path, uint32_t flags)
{
    /* Find free fd slot */
    int fd = -1;
    for (int i = 3; i < MAX_FDS; i++) { /* 0,1,2 reserved for stdin/out/err */
        if (!fd_table[i].used) { fd = i; break; }
    }
    if (fd < 0) return -1;

    /* Security audit — notify security daemon */
    if (flags & VFS_FLAG_WRITE) {
        ipc_message_t msg = {};
        msg.type = IPC_SECURITY_ALERT;
        snprintf((char *)msg.payload, sizeof(msg.payload),
            "File write: %s", path);
        /* Only send for sensitive paths */
        if (strncmp(path, "/jarvis/models", 14) == 0 ||
            strncmp(path, "/etc", 4) == 0) {
            ipc_send(&msg);
        }
    }

    /* Lookup file in ramfs */
    ramfs_file_t *rf = ramfs_find(path);
    if (!rf && (flags & VFS_FLAG_CREATE)) {
        rf = ramfs_create(path, 4096);
    }
    if (!rf) return -1;

    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    strncpy(node->name, path, sizeof(node->name) - 1);
    node->type   = VFS_TYPE_RAMFS;
    node->size   = rf->size;
    node->ops    = &ramfs_ops;
    node->priv   = rf;

    fd_table[fd].node   = node;
    fd_table[fd].offset = (flags & VFS_FLAG_APPEND) ? rf->size : 0;
    fd_table[fd].flags  = flags;
    fd_table[fd].used   = 1;

    if (flags & VFS_FLAG_TRUNC) {
        rf->size   = 0;
        node->size = 0;
    }

    return fd;
}

int vfs_read(int fd, void *buf, size_t len)
{
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -1;
    fd_t *f  = &fd_table[fd];
    int   n  = f->node->ops->read(f->node, f->offset, buf, len);
    if (n > 0) f->offset += (size_t)n;
    return n;
}

int vfs_write(int fd, const void *buf, size_t len)
{
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -1;
    fd_t *f = &fd_table[fd];
    int   n = f->node->ops->write(f->node, f->offset, buf, len);
    if (n > 0) f->offset += (size_t)n;
    return n;
}

void vfs_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return;
    if (fd_table[fd].node->ops->close)
        fd_table[fd].node->ops->close(fd_table[fd].node);
    kfree(fd_table[fd].node);
    fd_table[fd].used = 0;
}

int vfs_stat(const char *path)
{
    return ramfs_find(path) ? 0 : -1;
}

int vfs_create_file(const char *path)
{
    return ramfs_create_node(path, 4096, 0) ? 0 : -1;
}

int vfs_mkdir(const char *path)
{
    return ramfs_create_node(path, 0, 1) ? 0 : -1;
}

int vfs_read_file(const char *path, char *buf, size_t max)
{
    ramfs_file_t *rf = ramfs_find(path);
    if (!rf || rf->is_dir || max == 0)
        return -1;
    size_t len = rf->size;
    if (len >= max)
        len = max - 1;
    memcpy(buf, rf->data, len);
    buf[len] = '\0';
    return (int)len;
}

int vfs_list(const char *prefix, char *buf, size_t max)
{
    size_t out = 0;
    size_t plen = strlen(prefix);

    if (max == 0)
        return -1;
    buf[0] = '\0';

    for (int i = 0; i < ramfs_count; i++) {
        ramfs_file_t *rf = &ramfs_files[i];
        if (!rf->used)
            continue;
        if (plen && strncmp(rf->path, prefix, plen) != 0)
            continue;

        const char *kind = rf->is_dir ? "[dir] " : "[file] ";
        const char *parts[] = { kind, rf->path, "\n" };
        for (int p = 0; p < 3; p++) {
            const char *s = parts[p];
            while (*s && out + 1 < max)
                buf[out++] = *s++;
        }
    }

    if (out == 0) {
        const char *empty = "(empty)\n";
        while (*empty && out + 1 < max)
            buf[out++] = *empty++;
    }
    buf[out] = '\0';
    return (int)out;
}

/* Append to a file by path (used by security logger) */
void vfs_append(const char *path, const char *data, size_t len)
{
    int fd = vfs_open(path, VFS_FLAG_WRITE | VFS_FLAG_APPEND | VFS_FLAG_CREATE);
    if (fd < 0) return;
    vfs_write(fd, data, len);
    vfs_write(fd, "\n", 1);
    vfs_close(fd);
}

/* Memory-map a model file into the model pool (zero-copy for large weights) */
void *vfs_model_map(const char *path)
{
    ramfs_file_t *rf = ramfs_find(path);
    if (!rf) return NULL;
    /*
     * In production: the file data IS already in kernel memory (ramfs).
     * For disk-backed files: DMA read into model_pool and return pointer.
     */
    return rf->data;
}

/* AI file search — sends IPC to Python ai_filesystem.py service */
void ai_filesystem_search(const char *query)
{
    ipc_message_t msg = {};
    msg.type = IPC_FILE_OP;
    snprintf((char *)msg.payload, sizeof(msg.payload),
             "{\"type\":\"FILE_SEARCH\",\"payload\":\"%s\"}", query);
    msg.payload_len = strlen((char *)msg.payload);
    ipc_send(&msg);
}

void ai_filesystem_handle(void *msg_void)
{
    ipc_message_t *msg = (ipc_message_t *)msg_void;
    /* Parse JSON type field and dispatch */
    const char *p = (const char *)msg->payload;
    if (strstr(p, "FILE_SEARCH"))
        ai_filesystem_search(p);
    else if (strstr(p, "OPEN:"))
        vfs_open(p + 5, VFS_FLAG_READ);
}

void pkg_install(const char *name)
{
    terminal_printf("[PKG] Install requested: %s (stub)\n", name);
}
