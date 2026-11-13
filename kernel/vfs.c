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
    int      dirty;   /* changed since it was last written to disk */
    int      disk_idx;/* its entry in the on-disk file table, -1 = none yet */
    uint32_t gen;     /* bumped on every change (detects a change mid-save) */
} ramfs_file_t;

/* On-disk entries whose file was deleted; cleared by the next disk sync. */
static int ramfs_deleted[512];
static int ramfs_deleted_count = 0;

static void ramfs_touch(ramfs_file_t *f)
{
    f->dirty = 1;
    f->gen++;
}

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
    if (strlen(path) >= sizeof(ramfs_files[0].path)) return NULL;

    /* Reuse a slot freed by vfs_delete() before growing the table. */
    ramfs_file_t *f = NULL;
    for (int i = 0; i < ramfs_count; i++) {
        if (!ramfs_files[i].used) { f = &ramfs_files[i]; break; }
    }
    if (!f) {
        if (ramfs_count >= RAMFS_MAX_FILES) return NULL;
        f = &ramfs_files[ramfs_count++];
    }
    memset(f, 0, sizeof(*f));
    f->disk_idx = -1;
    f->dirty = 1;
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
    ramfs_touch(f);
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

static int vfs_on_mount(const char *path);   /* defined with vfs_list() */

/* ---- Public VFS API ------------------------------------------------------ */

/* Open a file, returns fd or negative on error */
int vfs_open(const char *path, uint32_t flags)
{
    if (vfs_on_mount(path)) return -1;      /* use vfs_read_file for USB drives */
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
        ramfs_touch(rf);
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
    if (vfs_on_mount(path)) return -1;
    return ramfs_create_node(path, 4096, 0) ? 0 : -1;
}

int vfs_mkdir(const char *path)
{
    if (vfs_on_mount(path)) return -1;
    return ramfs_create_node(path, 0, 1) ? 0 : -1;
}

int vfs_read_file(const char *path, char *buf, size_t max)
{
    if (vfs_on_mount(path)) {
        if (max == 0) return -1;
        int n = fatfs_read(path, 0, buf, max - 1);
        if (n < 0) return -1;
        buf[n] = '\0';
        return n;
    }
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

/* Folders backed by another file system (USB drives, kernel/fatfs.c). They
 * are listed as folders here but never stored in the ramfs, so nothing about
 * them is saved to the JARVIS disk. */
static char vfs_mounts[4][16];

void vfs_mount_point(const char *path)
{
    for (int i = 0; i < 4; i++)
        if (!vfs_mounts[i][0]) { strncpy(vfs_mounts[i], path, sizeof(vfs_mounts[i]) - 1); return; }
}

void vfs_unmount_point(const char *path)
{
    for (int i = 0; i < 4; i++)
        if (!strcmp(vfs_mounts[i], path)) vfs_mounts[i][0] = 0;
}

/* 1 if path is on a read-only mounted volume (writes must be refused). */
static int vfs_on_mount(const char *path)
{
    return path && fatfs_owns(path);
}

int vfs_list(const char *prefix, char *buf, size_t max)
{
    size_t out = 0;
    size_t plen = strlen(prefix);

    if (max == 0)
        return -1;
    if (vfs_on_mount(prefix))
        return fatfs_list(prefix, buf, max);
    buf[0] = '\0';

    for (int m = 0; m < 4; m++) {
        if (!vfs_mounts[m][0] || strncmp(vfs_mounts[m], prefix, plen) != 0) continue;
        const char *parts[] = { "[dir] ", vfs_mounts[m], "\n" };
        for (int p = 0; p < 3; p++)
            for (const char *t = parts[p]; *t && out + 1 < max; t++) buf[out++] = *t;
    }

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

/* ==========================================================================
 * File management: used by the Files app, Notepad and the terminal.
 *
 * The ramfs is a flat table of full paths, so a directory "contains" every
 * entry whose path starts with "<dir>/". A directory may also exist only
 * implicitly, as the prefix of deeper entries (e.g. /jarvis/models).
 * ========================================================================== */

/* 1 if `path` is `dir` itself or lies anywhere beneath it. */
static int path_is_under(const char *path, const char *dir, size_t dlen)
{
    return strncmp(path, dir, dlen) == 0 && (path[dlen] == '/' || path[dlen] == '\0');
}

static void ramfs_free_node(ramfs_file_t *f)
{
    if (f->disk_idx >= 0 && ramfs_deleted_count < 512)
        ramfs_deleted[ramfs_deleted_count++] = f->disk_idx;
    if (f->data) {
        if (f->large) kfree_large(f->data, f->capacity);
        else          kfree(f->data);
    }
    memset(f, 0, sizeof(*f));
}

int vfs_exists(const char *path)
{
    if (vfs_on_mount(path)) return fatfs_stat(path, NULL, NULL) == 0;
    size_t plen = strlen(path);
    for (int i = 0; i < ramfs_count; i++)
        if (ramfs_files[i].used && path_is_under(ramfs_files[i].path, path, plen))
            return 1;
    return 0;
}

int vfs_is_dir(const char *path)
{
    if (vfs_on_mount(path)) {
        int d = 0;
        return fatfs_stat(path, &d, NULL) == 0 && d;
    }
    ramfs_file_t *rf = ramfs_find(path);
    if (rf) return rf->is_dir;

    /* implicit directory: something lives beneath it */
    size_t plen = strlen(path);
    for (int i = 0; i < ramfs_count; i++)
        if (ramfs_files[i].used && strncmp(ramfs_files[i].path, path, plen) == 0 &&
            ramfs_files[i].path[plen] == '/')
            return 1;
    return 0;
}

/* Size in bytes of a file, or the total of everything under a directory. */
long vfs_size(const char *path)
{
    if (vfs_on_mount(path)) {
        uint32_t sz;
        return fatfs_stat(path, NULL, &sz) == 0 ? (long)sz : -1;
    }
    size_t plen = strlen(path);
    long total = -1;
    for (int i = 0; i < ramfs_count; i++) {
        ramfs_file_t *f = &ramfs_files[i];
        if (!f->used || !path_is_under(f->path, path, plen)) continue;
        if (total < 0) total = 0;
        if (!f->is_dir) total += (long)f->size;
    }
    return total;
}

/* Number of entries beneath a directory (all levels). */
int vfs_count_children(const char *path)
{
    if (vfs_on_mount(path)) {
        static char listing[8192];
        int n = 0;
        if (fatfs_list(path, listing, sizeof(listing)) < 0) return 0;
        for (const char *p = listing; *p; p++) if (*p == '\n') n++;
        return n;
    }
    size_t plen = strlen(path);
    int n = 0;
    for (int i = 0; i < ramfs_count; i++)
        if (ramfs_files[i].used && strncmp(ramfs_files[i].path, path, plen) == 0 &&
            ramfs_files[i].path[plen] == '/')
            n++;
    return n;
}

/* Delete a file, or a directory together with everything inside it. */
int vfs_delete(const char *path)
{
    if (vfs_on_mount(path)) return -1;              /* USB drives are read-only */
    size_t plen = strlen(path);
    int removed = 0;

    if (plen < 2) return -1;                    /* never the root */
    for (int i = 0; i < ramfs_count; i++) {
        ramfs_file_t *f = &ramfs_files[i];
        if (f->used && path_is_under(f->path, path, plen)) {
            ramfs_free_node(f);
            removed++;
        }
    }
    return removed ? 0 : -1;
}

/* Rename / move a file or a whole directory tree. */
int vfs_rename(const char *from, const char *to)
{
    if (vfs_on_mount(from) || vfs_on_mount(to)) return -1;
    size_t flen = strlen(from), tlen = strlen(to);

    if (flen < 2 || tlen < 2 || strcmp(from, to) == 0) return -1;
    if (!vfs_exists(from) || vfs_exists(to)) return -1;
    if (path_is_under(to, from, flen)) return -1;   /* into itself */

    /* Check every new path fits before changing anything. */
    for (int i = 0; i < ramfs_count; i++) {
        ramfs_file_t *f = &ramfs_files[i];
        if (f->used && path_is_under(f->path, from, flen) &&
            strlen(f->path) - flen + tlen >= sizeof(f->path))
            return -1;
    }

    for (int i = 0; i < ramfs_count; i++) {
        ramfs_file_t *f = &ramfs_files[i];
        if (!f->used || !path_is_under(f->path, from, flen)) continue;

        char tail[sizeof(f->path)];
        strncpy(tail, f->path + flen, sizeof(tail) - 1);
        tail[sizeof(tail) - 1] = '\0';
        snprintf(f->path, sizeof(f->path), "%s%s", to, tail);
        ramfs_touch(f);
    }
    return 0;
}

/* Copy a file, or a directory tree, to a new path. */
/* Copy a file or folder from a mounted USB drive onto the JARVIS disk. */
static int copy_from_mount(const char *from, const char *to, int depth)
{
    int is_dir = 0;
    uint32_t size = 0;
    if (depth > 16 || fatfs_stat(from, &is_dir, &size) != 0) return -1;

    if (!is_dir) {
        if (size > 256u * 1024 * 1024) return -1;           /* RAM-backed: keep it sane */
        uint8_t *buf = size >= LARGE_ALLOC_THRESHOLD ? (uint8_t *)kmalloc_large(size ? size : 1)
                                                     : (uint8_t *)kmalloc(size ? size : 1);
        if (!buf) return -1;
        int n = fatfs_read(from, 0, buf, size);
        int rc = (n == (int)size) ? vfs_write_file(to, buf, size) : -1;
        if (size >= LARGE_ALLOC_THRESHOLD) kfree_large(buf, size); else kfree(buf);
        return rc;
    }

    if (vfs_mkdir(to) != 0) return -1;
    char *listing = (char *)kmalloc(16384);
    if (!listing) return -1;
    int rc = fatfs_list(from, listing, 16384) < 0 ? -1 : 0;
    for (char *p = listing; rc == 0 && *p;) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *child = strchr(p, '/');                      /* full path after "[dir] " */
        const char *leaf = child ? strrchr(child, '/') + 1 : NULL;
        if (child && leaf && *leaf) {
            char dst[256];
            snprintf(dst, sizeof(dst), "%s/%s", to, leaf);
            rc = copy_from_mount(child, dst, depth + 1);
        }
        if (!eol) break;
        p = eol + 1;
    }
    kfree(listing);
    return rc;
}

int vfs_copy(const char *from, const char *to)
{
    if (vfs_on_mount(to)) return -1;
    if (vfs_on_mount(from)) return vfs_exists(to) ? -1 : copy_from_mount(from, to, 0);
    size_t flen = strlen(from);
    int count = ramfs_count;            /* don't revisit the copies we add */
    int copied = 0;

    if (!vfs_exists(from) || vfs_exists(to)) return -1;
    if (path_is_under(to, from, flen)) return -1;

    for (int i = 0; i < count; i++) {
        ramfs_file_t *src = &ramfs_files[i];
        if (!src->used || !path_is_under(src->path, from, flen)) continue;

        char dst_path[sizeof(src->path)];
        if (strlen(src->path) - flen + strlen(to) >= sizeof(dst_path)) return -1;
        snprintf(dst_path, sizeof(dst_path), "%s%s", to, src->path + flen);

        size_t size = src->size;
        int is_dir = src->is_dir;
        ramfs_file_t *dst = ramfs_create_node(dst_path, is_dir ? 0 : size, is_dir);
        if (!dst || !dst->data) return -1;

        src = &ramfs_files[i];
        if (!is_dir && size) {
            memcpy(dst->data, src->data, size);
            dst->size = size;
        }
        ramfs_touch(dst);
        copied++;
    }

    /* Copying an implicit directory still has to produce a directory. */
    if (!ramfs_find(to) && vfs_is_dir(from))
        ramfs_create_node(to, 0, 1);
    return copied || vfs_exists(to) ? 0 : -1;
}

/* Replace the contents of a file (created if missing). */
int vfs_write_file(const char *path, const void *data, size_t len)
{
    if (vfs_on_mount(path)) return -1;
    int fd = vfs_open(path, VFS_FLAG_WRITE | VFS_FLAG_CREATE | VFS_FLAG_TRUNC);
    if (fd < 0) return -1;

    ramfs_file_t *rf = ramfs_find(path);
    if (rf) { rf->size = 0; ramfs_touch(rf); }

    int n = len ? vfs_write(fd, data, len) : 0;
    vfs_close(fd);
    return n < 0 ? -1 : 0;
}

/* ==========================================================================
 * Interface for the on-disk file system (kernel/diskfs.c)
 * ========================================================================== */
static inline uint64_t vfs_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void vfs_irq_restore(uint64_t flags)
{
    if (flags & 0x200) __asm__ volatile ("sti");
}

int vfs_slot_max(void)
{
    return ramfs_count;
}

int vfs_slot_info(int slot, vfs_slot_info_t *out)
{
    if (slot < 0 || slot >= ramfs_count || !ramfs_files[slot].used) return -1;
    uint64_t irq = vfs_irq_save();
    ramfs_file_t *f = &ramfs_files[slot];
    strncpy(out->path, f->path, sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    out->size = f->size;
    out->is_dir = f->is_dir;
    out->dirty = f->dirty;
    out->disk_idx = f->disk_idx;
    out->gen = f->gen;
    vfs_irq_restore(irq);
    return 0;
}

/* Copy part of a file for saving. Fails (-1) if the file changed since
 * `gen` was read, so a save never mixes two versions of a file. */
int vfs_slot_read(int slot, uint64_t offset, void *buf, size_t len, uint32_t gen)
{
    if (slot < 0 || slot >= ramfs_count) return -1;
    uint64_t irq = vfs_irq_save();
    ramfs_file_t *f = &ramfs_files[slot];
    int n = -1;
    if (f->used && f->gen == gen) {
        size_t avail = offset < f->size ? f->size - (size_t)offset : 0;
        if (len > avail) len = avail;
        memcpy(buf, f->data + offset, len);
        n = (int)len;
    }
    vfs_irq_restore(irq);
    return n;
}

void vfs_slot_synced(int slot, int disk_idx, uint32_t gen)
{
    if (slot < 0 || slot >= ramfs_count) return;
    uint64_t irq = vfs_irq_save();
    ramfs_file_t *f = &ramfs_files[slot];
    if (f->used) {
        f->disk_idx = disk_idx;
        if (f->gen == gen) f->dirty = 0;    /* else: changed again, save again */
    }
    vfs_irq_restore(irq);
}

int vfs_take_deleted(int *disk_idx)
{
    uint64_t irq = vfs_irq_save();
    int had = ramfs_deleted_count > 0;
    if (had) *disk_idx = ramfs_deleted[--ramfs_deleted_count];
    vfs_irq_restore(irq);
    return had;
}

/* Recreate a file or folder read back from disk. Returns the buffer to fill
 * with `size` bytes of content (NULL on failure). */
uint8_t *vfs_restore_node(const char *path, int is_dir, size_t size, int disk_idx)
{
    ramfs_file_t *f = ramfs_create_node(path, size ? size : 4096, is_dir);
    if (!f || !f->data) return NULL;

    if (f->capacity < size) {               /* existed already, but smaller */
        uint8_t *bigger = size >= LARGE_ALLOC_THRESHOLD ? (uint8_t *)kmalloc_large(size)
                                                        : (uint8_t *)kmalloc(size);
        if (!bigger) return NULL;
        if (f->large) kfree_large(f->data, f->capacity); else kfree(f->data);
        f->data = bigger;
        f->capacity = size;
        f->large = size >= LARGE_ALLOC_THRESHOLD;
    }
    f->is_dir = is_dir;
    f->size = is_dir ? 0 : size;
    f->disk_idx = disk_idx;
    f->dirty = 0;
    return f->data;
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
