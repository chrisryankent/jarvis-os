/*
 * =============================================================================
 * JARVIS AI OS — Linux compatibility layer (kernel/linux.c)
 * Language: C17
 *
 * Runs unmodified Linux x86-64 programs, the way WSL1 does on Windows: the
 * program's system calls are carried out by the JARVIS kernel.
 *
 *   loader      ELF64 position-independent programs (PIE) and static-PIE;
 *               dynamically linked programs get their interpreter
 *               (ld-linux-x86-64.so.2), which then loads the shared
 *               libraries with mmap. The initial stack carries argv, envp
 *               and the auxiliary vector exactly as Linux lays it out.
 *   files       a Linux root at /jarvis/linux (/lib, /usr, /etc, /tmp ...);
 *               paths under /jarvis are the JARVIS files themselves;
 *               /dev/null, /dev/zero, /dev/urandom, /dev/tty, /proc/self/exe
 *   memory      mmap (anonymous and file), munmap, mprotect, brk, with
 *               demand paging; arch_prctl sets the FS base for thread-local
 *               storage (switched per process by the scheduler)
 *   system calls about 90 of the x86-64 Linux calls (see linux_syscall);
 *               anything else returns ENOSYS and is reported once in the log
 *
 * Not supported yet: fork/clone/execve (one process per program),
 * threads, sockets, signals being delivered (handlers are accepted),
 * programs linked at a fixed low address (non-PIE executables).
 * =============================================================================
 */

#include "kernel.h"
#include "crypto.h"
#include <string.h>
#include <stdio.h>

void usermode_enter(uint64_t entry, uint64_t user_stack);
void proc_set_fs_base(uint64_t v);
uint64_t proc_get_fs_base(void);

#define LX_ROOT        "/jarvis/linux"
#define LX_MAX         16
#define LX_FDS         64
#define LX_MAIN_BASE   0x0000555555554000ULL
#define LX_INTERP_BASE 0x00007FFE00000000ULL
#define LX_MMAP_TOP    0x00007FF000000000ULL
#define LX_STACK_TOP   0x00007FFFFFFFF000ULL
#define LX_STACK_PAGES 2048                 /* 8 MB, demand paged */
#define LX_BRK_PAGES   262144               /* 1 GB of address space for brk */

#define EPERM 1
#define ENOENT 2
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39
#define ECHILD 10
#define EAFNOSUPPORT 97

enum { FD_FREE = 0, FD_STDIN, FD_STDOUT, FD_FILE, FD_FATFILE, FD_DIR, FD_NULL, FD_ZERO, FD_RANDOM };

typedef struct {
    int      type;
    int      vfd;                   /* FD_FILE: VFS descriptor */
    int      flags;                 /* open flags */
    int      cloexec;
    uint64_t off;
    int      dirpos;                /* FD_DIR: next entry */
    char     path[256];             /* JARVIS path */
} lx_fd_t;

typedef struct {
    uint32_t pid;
    int      space;
    lx_fd_t  fd[LX_FDS];
    char     cwd[256];              /* JARVIS path */
    char     exe[256];              /* JARVIS path */
    uint64_t brk_start, brk_cur;
    uint64_t mmap_top;
    char     args[4096];            /* argv strings, NUL-separated */
    int      argc;
    uint32_t warned[8];             /* unsupported calls already reported */
    int      nwarned;
} lx_proc_t;

static lx_proc_t g_lx[LX_MAX];

static lx_proc_t *lx_cur(void)
{
    uint32_t pid = kthread_current_pid();
    for (int i = 0; i < LX_MAX; i++) if (g_lx[i].pid == pid) return &g_lx[i];
    return NULL;
}

int linux_is_current(void) { return lx_cur() != NULL; }

/* ---- paths ---------------------------------------------------------------------- */
static void lx_norm(char *p)
{
    char tmp[512], *parts[96];
    int np = 0;
    snprintf(tmp, sizeof(tmp), "%s", p);
    char *s = tmp;
    while (*s) {
        while (*s == '/') *s++ = 0;
        if (!*s) break;
        char *st = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (!strcmp(st, ".")) continue;
        if (!strcmp(st, "..")) { if (np) np--; continue; }
        if (np < 96) parts[np++] = st;
    }
    size_t o = 0;
    for (int i = 0; i < np && o < 250; i++) o += (size_t)snprintf(p + o, 256 - o, "/%s", parts[i]);
    if (!o) { p[0] = '/'; p[1] = 0; }
}

/* ---- merged /usr and symbolic links --------------------------------------------
 * The Linux root follows current Debian: /bin, /sbin, /lib, /lib64 are
 * aliases of the folders under /usr. Symbolic links (from packages, or made
 * by programs with symlink()) are kept in a table saved at
 * /jarvis/linux/var/lib/dpkg/jarvis-symlinks; the JARVIS file system itself
 * has no links. Paths are resolved component by component, like Linux. */
#define LX_LINKS     8192
#define LX_LINK_FILE LX_ROOT "/var/lib/dpkg/jarvis-symlinks"

typedef struct { char path[200]; char target[200]; int next; } lx_link_t;
static lx_link_t g_link[LX_LINKS];
static int g_link_bucket[4096], g_nlinks, g_links_loaded, g_links_dirty;

static const char *const g_usr_alias[] = { "bin", "sbin", "lib", "lib32", "lib64", "libx32" };

static uint32_t link_hash(const char *p)
{
    uint32_t h = 2166136261u;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h & 4095;
}

static void links_index(void)
{
    memset(g_link_bucket, 0, sizeof(g_link_bucket));
    for (int i = 0; i < g_nlinks; i++) {
        uint32_t b = link_hash(g_link[i].path);
        g_link[i].next = g_link_bucket[b];
        g_link_bucket[b] = i + 1;
    }
}

static void links_load(void)
{
    if (g_links_loaded) return;
    g_links_loaded = 1;
    g_nlinks = 0;
    long size = vfs_size(LX_LINK_FILE);
    if (size > 0) {
        char *buf = (char *)(size + 1 >= (long)LARGE_ALLOC_THRESHOLD ? kmalloc_large((size_t)size + 1) : kmalloc((size_t)size + 1));
        if (buf) {
            vfs_read_file(LX_LINK_FILE, buf, (size_t)size + 1);
            for (char *p = buf; *p && g_nlinks < LX_LINKS;) {
                char *eol = strchr(p, '\n');
                if (!eol) eol = p + strlen(p);
                char *tab = memchr(p, '\t', (size_t)(eol - p));
                if (tab && tab - p < 200 && eol - tab - 1 < 200 && p[0] == '/') {
                    lx_link_t *l = &g_link[g_nlinks++];
                    memcpy(l->path, p, (size_t)(tab - p)); l->path[tab - p] = 0;
                    memcpy(l->target, tab + 1, (size_t)(eol - tab - 1)); l->target[eol - tab - 1] = 0;
                }
                p = *eol ? eol + 1 : eol;
            }
            if (size + 1 >= (long)LARGE_ALLOC_THRESHOLD) kfree_large(buf, (size_t)size + 1); else kfree(buf);
        }
    }
    links_index();
}

/* Writes the table out if it changed. */
void linux_links_save(void)
{
    if (!g_links_dirty) return;
    size_t cap = (size_t)g_nlinks * 402 + 1;
    char *buf = (char *)(cap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(cap) : kmalloc(cap));
    if (!buf) return;
    size_t o = 0;
    for (int i = 0; i < g_nlinks; i++) o += (size_t)snprintf(buf + o, cap - o, "%s\t%s\n", g_link[i].path, g_link[i].target);
    vfs_write_file(LX_LINK_FILE, buf, o);
    if (cap >= LARGE_ALLOC_THRESHOLD) kfree_large(buf, cap); else kfree(buf);
    g_links_dirty = 0;
}

static int link_find(const char *path)
{
    links_load();
    for (int i = g_link_bucket[link_hash(path)]; i; i = g_link[i - 1].next)
        if (!strcmp(g_link[i - 1].path, path)) return i - 1;
    return -1;
}

/* The target of a symbolic link (Linux path), or NULL. The merged-/usr
 * folders read as links to usr/... the way they do on Debian. */
const char *linux_link_get(const char *path)
{
    static char alias[16];
    for (size_t i = 0; i < sizeof(g_usr_alias) / sizeof(g_usr_alias[0]); i++)
        if (path[0] == '/' && !strcmp(path + 1, g_usr_alias[i])) { snprintf(alias, sizeof(alias), "usr/%s", g_usr_alias[i]); return alias; }
    int i = link_find(path);
    return i >= 0 ? g_link[i].target : NULL;
}

int linux_link_set(const char *path, const char *target)
{
    if (strlen(path) >= 200 || strlen(target) >= 200 || path[0] != '/') return -1;
    int i = link_find(path);
    if (i >= 0) { snprintf(g_link[i].target, sizeof(g_link[i].target), "%s", target); g_links_dirty = 1; return 0; }
    if (g_nlinks >= LX_LINKS) return -1;
    lx_link_t *l = &g_link[g_nlinks];
    snprintf(l->path, sizeof(l->path), "%s", path);
    snprintf(l->target, sizeof(l->target), "%s", target);
    uint32_t b = link_hash(path);
    l->next = g_link_bucket[b];
    g_link_bucket[b] = ++g_nlinks;
    g_links_dirty = 1;
    return 0;
}

int linux_link_remove(const char *path)
{
    int i = link_find(path);
    if (i < 0) return -1;
    g_link[i] = g_link[--g_nlinks];
    links_index();
    g_links_dirty = 1;
    return 0;
}

/* Calls fn for every link directly inside the Linux folder `dir`. */
void linux_links_in(const char *dir, void (*fn)(void *ctx, const char *name, const char *target), void *ctx)
{
    links_load();
    size_t dl = strcmp(dir, "/") ? strlen(dir) : 0;
    if (!dl)
        for (size_t i = 0; i < sizeof(g_usr_alias) / sizeof(g_usr_alias[0]); i++) {
            char t[16];
            snprintf(t, sizeof(t), "usr/%s", g_usr_alias[i]);
            char d[200];
            snprintf(d, sizeof(d), "%s/usr/%s", LX_ROOT, g_usr_alias[i]);
            if (vfs_is_dir(d)) fn(ctx, g_usr_alias[i], t);
        }
    for (int i = 0; i < g_nlinks; i++) {
        const char *p = g_link[i].path;
        if (!strncmp(p, dir, dl) && p[dl] == '/' && p[dl + 1] && !strchr(p + dl + 1, '/')) fn(ctx, p + dl + 1, g_link[i].target);
    }
}

/* Resolves a normalized absolute Linux path: merged /usr, then symbolic
 * links in every component (and the last one too, if follow_last). */
void linux_canon(const char *in, char *out, int follow_last)
{
    char cur[512];
    snprintf(cur, sizeof(cur), "%s", in);
    lx_norm(cur);
    for (int iter = 0; iter < 40; iter++) {
        /* /bin/... -> /usr/bin/... */
        for (size_t i = 0; i < sizeof(g_usr_alias) / sizeof(g_usr_alias[0]); i++) {
            size_t l = strlen(g_usr_alias[i]);
            if (cur[0] == '/' && !strncmp(cur + 1, g_usr_alias[i], l) && (cur[1 + l] == 0 || cur[1 + l] == '/')) {
                char t[512];
                snprintf(t, sizeof(t), "/usr%s", cur);
                snprintf(cur, sizeof(cur), "%s", t);
                break;
            }
        }
        if (!strncmp(cur, "/jarvis", 7) && (cur[7] == 0 || cur[7] == '/')) break;
        int changed = 0;
        size_t n = strlen(cur);
        for (size_t e = 1; e <= n && !changed; e++) {
            if (e < n && cur[e] != '/') continue;
            if (e == n && !follow_last) break;
            char prefix[512];
            memcpy(prefix, cur, e);
            prefix[e] = 0;
            int li = link_find(prefix);
            if (li < 0) continue;
            const char *t = g_link[li].target;
            char next[1024];
            if (t[0] == '/') snprintf(next, sizeof(next), "%s%s", t, cur + e);
            else {
                char *slash = strrchr(prefix, '/');
                *slash = 0;
                snprintf(next, sizeof(next), "%s/%s%s", prefix, t, cur + e);
            }
            snprintf(cur, sizeof(cur), "%.511s", next);
            lx_norm(cur);
            changed = 1;
        }
        if (!changed) break;
    }
    snprintf(out, 256, "%s", cur);
}

/* Absolute, normalized Linux path for a path a program passed in. */
static void lx_abs(lx_proc_t *L, int dirfd, const char *in, char *full)
{
    if (in[0] == '/') snprintf(full, 512, "%s", in);
    else {
        const char *base = L->cwd;
        if (dirfd >= 0 && dirfd < LX_FDS && L->fd[dirfd].type == FD_DIR) base = L->fd[dirfd].path;
        /* the base is a JARVIS path: turn it back into the Linux view */
        if (!strncmp(base, LX_ROOT, strlen(LX_ROOT))) {
            const char *rest = base + strlen(LX_ROOT);
            snprintf(full, 512, "%s/%s", *rest ? rest : "", in);
        } else snprintf(full, 512, "%s/%s", base, in);
    }
    lx_norm(full);
}

/* Linux path (as the program sees it) -> JARVIS path. Returns a special
 * file type, or 0 for an ordinary path. */
static int lx_path_f(lx_proc_t *L, int dirfd, const char *in, char *out, int follow)
{
    char full[512];
    lx_abs(L, dirfd, in, full);
    linux_canon(full, full, follow);
    if (!strcmp(full, "/dev/null")) { snprintf(out, 256, "%s", full); return FD_NULL; }
    if (!strcmp(full, "/dev/zero")) { snprintf(out, 256, "%s", full); return FD_ZERO; }
    if (!strcmp(full, "/dev/urandom") || !strcmp(full, "/dev/random")) { snprintf(out, 256, "%s", full); return FD_RANDOM; }
    if (!strcmp(full, "/dev/tty") || !strcmp(full, "/dev/stdout") || !strcmp(full, "/dev/stderr")) { snprintf(out, 256, "%s", full); return FD_STDOUT; }
    if (!strcmp(full, "/dev/stdin")) { snprintf(out, 256, "%s", full); return FD_STDIN; }
    if (!strcmp(full, "/proc/self/exe")) { snprintf(out, 256, "%s", L->exe); return 0; }
    if (!strncmp(full, "/jarvis", 7) && (full[7] == 0 || full[7] == '/')) { snprintf(out, 256, "%s", full); return 0; }
    snprintf(out, 256, "%s%s", LX_ROOT, strcmp(full, "/") ? full : "");
    return 0;
}

static int lx_path(lx_proc_t *L, int dirfd, const char *in, char *out) { return lx_path_f(L, dirfd, in, out, 1); }

/* Is `in` (not followed) a symbolic link? Fills its Linux path. */
static const char *lx_is_link(lx_proc_t *L, int dirfd, const char *in, char *linux_path)
{
    char full[512];
    lx_abs(L, dirfd, in, full);
    if (full[0] == '/' && full[1] && !strchr(full + 1, '/') && linux_link_get(full)) {   /* /bin, /lib ... */
        snprintf(linux_path, 256, "%s", full);
        return linux_link_get(full);
    }
    /* resolve the folders, but not the last component */
    linux_canon(full, linux_path, 0);
    return linux_link_get(linux_path);
}

/* JARVIS path -> what the program should see */
static void lx_view(const char *jp, char *out, size_t n)
{
    size_t rl = strlen(LX_ROOT);
    if (!strncmp(jp, LX_ROOT, rl) && (jp[rl] == 0 || jp[rl] == '/')) snprintf(out, n, "%s", jp[rl] ? jp + rl : "/");
    else snprintf(out, n, "%s", jp);
}

/* ---- user memory --------------------------------------------------------------------- */
static int uok(uint64_t a, uint64_t len, int w) { return len == 0 || vm_user_range_ok(proc_current_space(), a, len, w); }

static int ustr(char *dst, size_t max, uint64_t src)
{
    for (size_t i = 0; i < max; i++) {
        if ((i == 0 || ((src + i) & 0xFFF) == 0) && !uok(src + i, 1, 0)) return -1;
        dst[i] = *(const char *)(src + i);
        if (!dst[i]) return 0;
    }
    return -1;
}

/* ---- files ---------------------------------------------------------------------------- */
static int fd_alloc(lx_proc_t *L, int from)
{
    for (int i = from; i < LX_FDS; i++) if (L->fd[i].type == FD_FREE) return i;
    return -EMFILE;
}

static void fd_close(lx_proc_t *L, int fd)
{
    lx_fd_t *f = &L->fd[fd];
    if (f->type == FD_FILE && f->vfd >= 0) {
        /* other descriptors may share the VFS file (dup) */
        int shared = 0;
        for (int i = 0; i < LX_FDS; i++) if (i != fd && L->fd[i].type == FD_FILE && L->fd[i].vfd == f->vfd) shared = 1;
        if (!shared) vfs_close(f->vfd);
    }
    memset(f, 0, sizeof(*f));
}

static int64_t lx_open(lx_proc_t *L, int dirfd, uint64_t upath, int flags)
{
    char lp[256], jp[256];
    if (ustr(lp, sizeof(lp), upath)) return -EFAULT;
    int special = lx_path(L, dirfd, lp, jp);
    if (vfs_is_private(jp)) return -EACCES;
    int fd = fd_alloc(L, 0);
    if (fd < 0) return fd;
    lx_fd_t *f = &L->fd[fd];
    memset(f, 0, sizeof(*f));
    f->flags = flags;
    f->cloexec = (flags & 0x80000) != 0;
    snprintf(f->path, sizeof(f->path), "%s", jp);
    if (special) { f->type = special; return fd; }
    int acc = flags & 3, creat = flags & 0x40, excl = flags & 0x80, trunc = flags & 0x200, append = flags & 0x400, dir = flags & 0x10000;
    int exists = vfs_exists(jp) || vfs_is_dir(jp);
    if (!strcmp(jp, LX_ROOT) || vfs_is_dir(jp)) {
        if (acc != 0) { memset(f, 0, sizeof(*f)); return -EISDIR; }
        f->type = FD_DIR;
        return fd;
    }
    if (dir) { memset(f, 0, sizeof(*f)); return exists ? -ENOTDIR : -ENOENT; }
    if (exists && creat && excl) { memset(f, 0, sizeof(*f)); return -EEXIST; }
    if (!exists && !creat) { memset(f, 0, sizeof(*f)); return -ENOENT; }
    if (fatfs_owns(jp)) {
        if (!exists || trunc) fatfs_write_file(jp, "", 0);
        f->type = FD_FATFILE;
        f->off = append ? (uint64_t)vfs_size(jp) : 0;
        return fd;
    }
    uint32_t vf = (acc == 0 ? 0x01 : acc == 1 ? 0x02 : 0x03) | (creat ? 0x04 : 0) | (append ? 0x08 : 0) | (trunc && acc ? 0x10 : 0);
    int v = vfs_open(jp, vf | 0x01);
    if (v < 0) { memset(f, 0, sizeof(*f)); return creat ? -EACCES : -ENOENT; }
    f->type = FD_FILE;
    f->vfd = v;
    if (append) vfs_lseek(v, 0, 2);
    return fd;
}

static int64_t fd_read(lx_proc_t *L, int fd, void *buf, size_t n, int64_t at)
{
    if (fd < 0 || fd >= LX_FDS) return -EBADF;
    lx_fd_t *f = &L->fd[fd];
    switch (f->type) {
    case FD_STDIN: { int r = user_stdin_read((char *)buf, n); return r < 0 ? 0 : r; }
    case FD_NULL: return 0;
    case FD_ZERO: memset(buf, 0, n); return (int64_t)n;
    case FD_RANDOM: crypto_random(buf, n); return (int64_t)n;
    case FD_FILE: {
        int64_t save = -1;
        if (at >= 0) { save = vfs_lseek(f->vfd, 0, 1); vfs_lseek(f->vfd, at, 0); }
        int r = vfs_read(f->vfd, buf, n);
        if (save >= 0) vfs_lseek(f->vfd, save, 0);
        return r < 0 ? -EBADF : r;
    }
    case FD_FATFILE: {
        uint64_t o = at >= 0 ? (uint64_t)at : f->off;
        int r = fatfs_read(f->path, o, buf, n);
        if (r < 0) return -EBADF;
        if (at < 0) f->off += (uint64_t)r;
        return r;
    }
    case FD_DIR: return -EISDIR;
    case FD_STDOUT: return 0;
    default: return -EBADF;
    }
}

static int64_t fd_write(lx_proc_t *L, int fd, const void *buf, size_t n, int64_t at)
{
    if (fd < 0 || fd >= LX_FDS) return -EBADF;
    lx_fd_t *f = &L->fd[fd];
    switch (f->type) {
    case FD_STDOUT: user_console_write((const char *)buf, n); return (int64_t)n;
    case FD_NULL: case FD_ZERO: case FD_RANDOM: return (int64_t)n;
    case FD_FILE: {
        int64_t save = -1;
        if (at >= 0) { save = vfs_lseek(f->vfd, 0, 1); vfs_lseek(f->vfd, at, 0); }
        int r = vfs_write(f->vfd, buf, n);
        if (save >= 0) vfs_lseek(f->vfd, save, 0);
        return r < 0 ? -EBADF : r;
    }
    case FD_FATFILE: {
        /* FAT files: append is native; writes elsewhere rewrite the file */
        uint64_t o = at >= 0 ? (uint64_t)at : f->off;
        long size = vfs_size(f->path);
        if (size >= 0 && o == (uint64_t)size) { if (fatfs_append(f->path, buf, n)) return -EBADF; }
        else {
            size_t nl = (size_t)(o + n > (uint64_t)size ? o + n : (uint64_t)size);
            uint8_t *tmp = (uint8_t *)(nl + 1 >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(nl + 1) : kmalloc(nl + 1));
            if (!tmp) return -ENOMEM;
            memset(tmp, 0, nl + 1);
            if (size > 0) fatfs_read(f->path, 0, tmp, (size_t)size);
            memcpy(tmp + o, buf, n);
            int rc = fatfs_write_file(f->path, tmp, nl);
            if (nl + 1 >= LARGE_ALLOC_THRESHOLD) kfree_large(tmp, nl + 1); else kfree(tmp);
            if (rc) return -EBADF;
        }
        if (at < 0) f->off = o + n;
        return (int64_t)n;
    }
    default: return -EBADF;
    }
}

/* struct stat (x86-64) */
typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t  reserved[3];
} lx_stat_t;

static uint64_t path_ino(const char *p)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    return h & 0x7FFFFFFFFFFFULL;
}

static int64_t stat_path(const char *jp, int special, lx_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev = 0x801;
    st->nlink = 1;
    st->blksize = 4096;
    st->atime = st->mtime = st->ctime = (uint64_t)sys_unix_time();
    st->ino = path_ino(jp);
    if (special) {
        st->mode = 020666;                                  /* character device */
        st->rdev = special == FD_NULL ? 0x103 : special == FD_ZERO ? 0x105 : 0x109;
        return 0;
    }
    if (!strcmp(jp, LX_ROOT) || vfs_is_dir(jp)) { st->mode = 040755; st->nlink = 2; st->size = 4096; return 0; }
    if (!vfs_exists(jp)) return -ENOENT;
    long sz = vfs_size(jp);
    st->size = sz < 0 ? 0 : sz;
    st->blocks = (st->size + 511) / 512;
    int exe = 0;
    if (strstr(jp, "/bin/") || strstr(jp, "/sbin/") || strstr(jp, "/libexec/") || strstr(jp, ".so")) exe = 1;
    else { char h[8]; exe = vfs_read_file(jp, h, sizeof(h)) >= 4 && h[0] == 0x7F && h[1] == 'E'; }
    st->mode = exe ? 0100755 : 0100644;
    return 0;
}

static void stat_link(const char *lp, const char *target, lx_stat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->dev = 0x801;
    st->nlink = 1;
    st->blksize = 4096;
    st->mode = 0120777;
    st->size = (int64_t)strlen(target);
    st->ino = path_ino(lp);
    st->atime = st->mtime = st->ctime = (uint64_t)sys_unix_time();
}

static int64_t stat_fd(lx_proc_t *L, int fd, lx_stat_t *st)
{
    if (fd < 0 || fd >= LX_FDS || L->fd[fd].type == FD_FREE) return -EBADF;
    lx_fd_t *f = &L->fd[fd];
    if (f->type == FD_STDIN || f->type == FD_STDOUT) {
        memset(st, 0, sizeof(*st));
        st->mode = 020620; st->rdev = 0x8800; st->blksize = 1024; st->nlink = 1; st->ino = (uint64_t)fd + 1;
        return 0;
    }
    if (f->type >= FD_NULL) return stat_path(f->path, f->type, st);
    return stat_path(f->path, 0, st);
}

/* ---- directories --------------------------------------------------------------------- */
/* The entries of one folder, gathered for getdents64: name pool + types. */
typedef struct { char *pool; size_t used, cap; int n, max; uint32_t *off; uint8_t *type; } dents_t;

static void dents_add(dents_t *d, const char *name, size_t nl, int type)
{
    if (d->n >= d->max || d->used + nl + 1 > d->cap) return;
    for (int i = 0; i < d->n; i++)                          /* a link and a file of the same name */
        if (!strncmp(d->pool + d->off[i], name, nl) && d->pool[d->off[i] + nl] == 0) return;
    d->off[d->n] = (uint32_t)d->used;
    d->type[d->n++] = (uint8_t)type;
    memcpy(d->pool + d->used, name, nl);
    d->pool[d->used + nl] = 0;
    d->used += nl + 1;
}

static void dents_vfs(void *ctx, const char *name, size_t nl, int is_dir) { dents_add((dents_t *)ctx, name, nl, is_dir ? 4 : 8); }
static void dents_link(void *ctx, const char *name, const char *target) { (void)target; dents_add((dents_t *)ctx, name, strlen(name), 10); }

static int64_t lx_getdents64(lx_proc_t *L, int fd, uint64_t ubuf, size_t count)
{
    if (fd < 0 || fd >= LX_FDS || L->fd[fd].type != FD_DIR) return -ENOTDIR;
    if (!uok(ubuf, count, 1)) return -EFAULT;
    lx_fd_t *f = &L->fd[fd];
    static char pool[262144];
    static uint32_t off[8192];
    static uint8_t type[8192];
    dents_t d = { pool, 0, sizeof(pool), 0, 8192, off, type };
    dents_add(&d, ".", 1, 4);
    dents_add(&d, "..", 2, 4);
    if (vfs_dir_entries(f->path, dents_vfs, &d) < 0) {
        /* a USB drive: its listing holds full paths, one per line */
        static char listing[32768];
        if (vfs_list(f->path, listing, sizeof(listing)) < 0) listing[0] = 0;
        size_t dl = strlen(f->path);
        for (const char *p = listing; *p;) {
            const char *eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            const char *full = strchr(p, '/');
            if (full && full < eol && (size_t)(eol - full) > dl + 1 && !strncmp(full, f->path, dl) && full[dl] == '/' &&
                !memchr(full + dl + 1, '/', (size_t)(eol - full) - dl - 1))
                dents_add(&d, full + dl + 1, (size_t)(eol - full) - dl - 1, !strncmp(p, "[dir]", 5) ? 4 : 8);
            p = *eol ? eol + 1 : eol;
        }
    } else if (!strncmp(f->path, LX_ROOT, strlen(LX_ROOT))) {
        char lv[256];
        lx_view(f->path, lv, sizeof(lv));
        linux_links_in(lv, dents_link, &d);
    }
    /* hide the protected system folder */
    if (!strcmp(f->path, "/jarvis"))
        for (int i = 0; i < d.n; i++) if (!strcmp(pool + off[i], "system")) type[i] = 0;
    size_t o = 0;
    uint8_t *out = (uint8_t *)ubuf;
    for (int i = f->dirpos; i < d.n; i++) {
        f->dirpos = i;
        if (!type[i]) continue;
        const char *name = pool + off[i];
        size_t nl = strlen(name);
        size_t rl = (19 + nl + 1 + 7) & ~7ULL;
        if (o + rl > count) return o ? (int64_t)o : -EINVAL;
        char fp[512];
        snprintf(fp, sizeof(fp), "%s/%s", f->path, name);
        uint64_t ino = path_ino(fp);
        int64_t next = i + 1;
        uint16_t r16 = (uint16_t)rl;
        memcpy(out + o, &ino, 8);
        memcpy(out + o + 8, &next, 8);
        memcpy(out + o + 16, &r16, 2);
        out[o + 18] = type[i];
        memcpy(out + o + 19, name, nl + 1);
        o += rl;
    }
    f->dirpos = d.n;
    return (int64_t)o;
}

/* ---- memory ------------------------------------------------------------------------------- */
static uint32_t prot_flags(int prot)
{
    return VM_USER | ((prot & 2) ? VM_WRITE : 0) | ((prot & 4) ? VM_EXEC : 0);
}

static int64_t lx_mmap(lx_proc_t *L, uint64_t addr, uint64_t len, int prot, int flags, int fd, int64_t off)
{
    if (!len || (off & 0xFFF)) return -EINVAL;
    uint64_t size = (len + 0xFFF) & ~0xFFFULL;
    uint32_t pages = (uint32_t)(size >> 12);
    int fixed = (flags & 0x10) || (flags & 0x100000);       /* MAP_FIXED, MAP_FIXED_NOREPLACE */
    int anon = (flags & 0x20) != 0;
    if (!anon && (fd < 0 || fd >= LX_FDS || (L->fd[fd].type != FD_FILE && L->fd[fd].type != FD_FATFILE && L->fd[fd].type != FD_ZERO))) return -EBADF;
    if (fixed) {
        if (addr & 0xFFF) return -EINVAL;
        if ((flags & 0x100000) && vm_range_used(L->space, addr, size)) return -EEXIST;
        vm_region_unmap(L->space, addr, pages);
    } else if (addr && !(addr & 0xFFF) && addr >= VM_USER_BASE && addr + size < LX_MMAP_TOP && !vm_range_used(L->space, addr, size)) {
        /* the hint is free: use it */
    } else {
        L->mmap_top -= size;
        while (vm_range_used(L->space, L->mmap_top, size)) L->mmap_top -= 0x100000;
        addr = L->mmap_top;
    }
    if (vm_region_add(L->space, addr, pages, VM_USER | VM_WRITE, anon ? "anon" : "file") != 0) return -ENOMEM;
    if (!anon && L->fd[fd].type != FD_ZERO) {
        /* copy the file's bytes in (private mappings: later writes stay in memory) */
        uint64_t done = 0;
        static uint8_t chunk[65536];
        while (done < len) {
            size_t k = len - done < sizeof(chunk) ? (size_t)(len - done) : sizeof(chunk);
            int64_t r = fd_read(L, fd, chunk, k, off + (int64_t)done);
            if (r <= 0) break;
            memcpy((void *)(addr + done), chunk, (size_t)r);
            done += (uint64_t)r;
            if ((size_t)r < k) break;
        }
    }
    vm_region_protect(L->space, addr, pages, prot_flags(prot));
    return (int64_t)addr;
}

/* ---- the program image ---------------------------------------------------------------------- */
typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ehdr_t;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } phdr_t;

static uint8_t *load_file(const char *jp, size_t *len)
{
    long size = vfs_size(jp);
    if (size <= 0 || vfs_is_dir(jp)) return NULL;
    size_t cap = (size_t)size + 1;
    uint8_t *b = (uint8_t *)(cap >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(cap) : kmalloc(cap));
    if (!b) return NULL;
    if (vfs_read_file(jp, (char *)b, cap) != size) { if (cap >= LARGE_ALLOC_THRESHOLD) kfree_large(b, cap); else kfree(b); return NULL; }
    *len = (size_t)size;
    return b;
}

static void free_file(uint8_t *b, size_t len)
{
    if (!b) return;
    if (len + 1 >= LARGE_ALLOC_THRESHOLD) kfree_large(b, len + 1); else kfree(b);
}

/* Maps an ELF image. Returns the load bias (0 for ET_EXEC), or -1. */
static int64_t map_elf(lx_proc_t *L, const uint8_t *img, size_t size, uint64_t want_base, uint64_t *end_out, const char **why)
{
    const ehdr_t *eh = (const ehdr_t *)img;
    if (size < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 0x3E) { *why = "not a 64-bit x86 program"; return -1; }
    if (eh->type != 2 && eh->type != 3) { *why = "not an executable"; return -1; }
    if (eh->phoff + (uint64_t)eh->phnum * sizeof(phdr_t) > size) { *why = "damaged program file"; return -1; }
    uint64_t bias = eh->type == 3 ? want_base : 0;
    uint64_t end = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const phdr_t *ph = (const phdr_t *)(img + eh->phoff + (uint64_t)i * sizeof(phdr_t));
        if (ph->type != 1 || !ph->memsz) continue;
        uint64_t va = bias + ph->vaddr;
        if (va < VM_USER_BASE) { *why = "linked at a fixed low address (non-PIE); rebuild it with -fPIE -pie or -static-pie"; return -1; }
        if (ph->offset + ph->filesz > size || ph->filesz > ph->memsz) { *why = "damaged program file"; return -1; }
        uint64_t start = va & ~0xFFFULL;
        uint64_t stop = (va + ph->memsz + 0xFFF) & ~0xFFFULL;
        uint32_t pages = (uint32_t)((stop - start) >> 12);
        vm_region_unmap(L->space, start, pages);
        if (vm_region_add(L->space, start, pages, VM_USER | VM_WRITE, "elf") != 0) { *why = "out of memory"; return -1; }
        memcpy((void *)va, img + ph->offset, ph->filesz);
        int prot = ((ph->flags & 4) ? 1 : 0) | ((ph->flags & 2) ? 2 : 0) | ((ph->flags & 1) ? 4 : 0);
        vm_region_protect(L->space, start, pages, prot_flags(prot));
        if (stop > end) end = stop;
    }
    if (end_out) *end_out = end;
    return (int64_t)bias;
}

/* ---- process start ---------------------------------------------------------------------------- */
static uint64_t push_bytes(uint64_t *sp, const void *d, size_t n)
{
    *sp -= n;
    memcpy((void *)*sp, d, n);
    return *sp;
}

static void lx_main(void)
{
    lx_proc_t *L = lx_cur();
    if (!L) return;
    char msg[300];
    const char *why = "could not be started";
    const char *leaf = strrchr(L->exe, '/');
    leaf = leaf ? leaf + 1 : L->exe;
    size_t mlen = 0, ilen = 0;
    uint8_t *img = load_file(L->exe, &mlen), *interp = NULL;
    uint64_t entry = 0, main_bias = 0, interp_bias = 0, end = 0, phdr = 0;
    const ehdr_t *eh = (const ehdr_t *)img;

    L->space = vm_space_create(L->pid);
    if (!img) why = "file could not be read";
    else if (L->space < 0 || proc_use_space(L->space) != 0) why = "not enough memory for a new address space";
    else {
        int64_t b = map_elf(L, img, mlen, LX_MAIN_BASE, &end, &why);
        if (b >= 0) {
            main_bias = (uint64_t)b;
            entry = main_bias + eh->entry;
            /* program headers in memory, and the interpreter if any */
            char ipath[256] = "";
            for (int i = 0; i < eh->phnum; i++) {
                const phdr_t *ph = (const phdr_t *)(img + eh->phoff + (uint64_t)i * sizeof(phdr_t));
                if (ph->type == 6) phdr = main_bias + ph->vaddr;
                if (ph->type == 3 && ph->filesz < sizeof(ipath)) { memcpy(ipath, img + ph->offset, ph->filesz); ipath[ph->filesz] = 0; }
                if (ph->type == 1 && !phdr && ph->offset == 0) phdr = main_bias + ph->vaddr + eh->phoff;
            }
            if (ipath[0]) {
                char jp[256];
                lx_path(L, -100, ipath, jp);
                interp = load_file(jp, &ilen);
                if (!interp) {
                    snprintf(msg, sizeof(msg), "needs %s (the Linux loader); install the Linux runtime: apt install libc6", ipath);
                    why = msg;
                    entry = 0;
                } else {
                    int64_t ib = map_elf(L, interp, ilen, LX_INTERP_BASE, NULL, &why);
                    if (ib < 0) entry = 0;
                    else { interp_bias = (uint64_t)ib; entry = interp_bias + ((const ehdr_t *)interp)->entry; }
                }
            }
        }
    }
    if (entry &&
        (vm_region_add(L->space, LX_STACK_TOP - (uint64_t)LX_STACK_PAGES * 4096, LX_STACK_PAGES, VM_USER | VM_WRITE, "stack") != 0 ||
         vm_region_add(L->space, (end + 0x10000 + 0xFFF) & ~0xFFFULL, LX_BRK_PAGES, VM_USER | VM_WRITE, "brk") != 0)) {
        why = "not enough memory";
        entry = 0;
    }
    if (!entry) {
        char line[400];
        snprintf(line, sizeof(line), "%s: %s\n", leaf, why);
        user_console_write(line, strlen(line));
        free_file(img, mlen);
        free_file(interp, ilen);
        L->pid = 0;
        return;
    }
    L->brk_start = L->brk_cur = (end + 0x10000 + 0xFFF) & ~0xFFFULL;
    L->mmap_top = LX_MMAP_TOP;

    /* the initial stack: strings, random bytes, auxv, envp, argv, argc */
    uint64_t sp = LX_STACK_TOP;
    uint64_t argv_p[64], envp_p[16];
    int argc = 0;
    for (const char *a = L->args; argc < L->argc && argc < 63; a += strlen(a) + 1) argv_p[argc++] = push_bytes(&sp, a, strlen(a) + 1);
    char cwdv[256], pwd[300], host[64];
    lx_view(L->cwd, cwdv, sizeof(cwdv));
    snprintf(pwd, sizeof(pwd), "PWD=%s", cwdv);
    setting_get("device_name", host, sizeof(host));
    char hostenv[96];
    snprintf(hostenv, sizeof(hostenv), "HOSTNAME=%s", host[0] ? host : "JARVIS-PC");
    const char *envs[] = { "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root", "USER=root",
                           "LOGNAME=root", "SHELL=/bin/sh", "TERM=dumb", "LANG=C.UTF-8", "COLUMNS=100", "LINES=30", pwd, hostenv };
    int envc = 0;
    for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]) && envc < 15; i++) envp_p[envc++] = push_bytes(&sp, envs[i], strlen(envs[i]) + 1);
    char execfn[256];
    lx_view(L->exe, execfn, sizeof(execfn));
    uint64_t execfn_p = push_bytes(&sp, execfn, strlen(execfn) + 1);
    uint64_t plat_p = push_bytes(&sp, "x86_64", 7);
    uint8_t rnd[16];
    crypto_random(rnd, 16);
    uint64_t rnd_p = push_bytes(&sp, rnd, 16);
    sp &= ~0xFULL;
    uint64_t aux[][2] = {
        { 3, phdr }, { 4, sizeof(phdr_t) }, { 5, eh->phnum }, { 6, 4096 }, { 7, interp_bias }, { 8, 0 },
        { 9, main_bias + eh->entry }, { 11, 0 }, { 12, 0 }, { 13, 0 }, { 14, 0 }, { 16, 0x078bfbff }, { 17, 100 },
        { 23, 0 }, { 25, rnd_p }, { 31, execfn_p }, { 15, plat_p }, { 0, 0 },
    };
    size_t words = 1 + (size_t)argc + 1 + (size_t)envc + 1 + sizeof(aux) / 8;
    if (words & 1) sp -= 8;                                 /* keep rsp 16-byte aligned at argc */
    sp -= sizeof(aux);
    memcpy((void *)sp, aux, sizeof(aux));
    sp -= 8; *(uint64_t *)sp = 0;
    for (int i = envc - 1; i >= 0; i--) { sp -= 8; *(uint64_t *)sp = envp_p[i]; }
    sp -= 8; *(uint64_t *)sp = 0;
    for (int i = argc - 1; i >= 0; i--) { sp -= 8; *(uint64_t *)sp = argv_p[i]; }
    sp -= 8; *(uint64_t *)sp = (uint64_t)argc;

    free_file(img, mlen);
    free_file(interp, ilen);
    proc_set_note("");
    proc_mark_user();
    usermode_enter(entry, sp);
}

/* Is this file a Linux program (rather than a native JARVIS one)? Native
 * programs are fixed-address ELF files linked inside JARVIS user memory. */
int linux_is_program(const char *path)
{
    uint8_t h[64];
    char b[65];
    if (vfs_read_file(path, b, sizeof(b)) < 64) return 0;
    memcpy(h, b, 64);
    const ehdr_t *eh = (const ehdr_t *)h;
    if (memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2) return 0;
    if (eh->type == 3) return 1;                            /* PIE / shared: Linux */
    if (eh->ident[7] == 3) return 1;                        /* ELFOSABI_LINUX */
    /* a fixed-address program below JARVIS user memory is a non-PIE Linux program */
    static uint8_t full[4096];
    char fb[4097];
    int n = vfs_read_file(path, fb, sizeof(fb));
    if (n < 64) return 0;
    memcpy(full, fb, (size_t)n);
    for (int i = 0; i < eh->phnum && eh->phoff + (uint64_t)(i + 1) * sizeof(phdr_t) <= (uint64_t)n; i++) {
        const phdr_t *ph = (const phdr_t *)(full + eh->phoff + (uint64_t)i * sizeof(phdr_t));
        if (ph->type == 3) return 1;                        /* has an interpreter */
        if (ph->type == 1 && ph->vaddr < VM_USER_BASE) return 1;
    }
    return 0;
}

/* Starts a Linux program. argv[0] is the program name. Returns the PID. */
uint32_t linux_exec(const char *path, int argc, char **argv, const char *cwd)
{
    int s = -1;
    for (int i = 0; i < LX_MAX; i++) if (!g_lx[i].pid || !proc_exists(g_lx[i].pid)) { s = i; break; }
    if (s < 0) return 0;
    lx_proc_t *L = &g_lx[s];
    memset(L, 0, sizeof(*L));
    snprintf(L->exe, sizeof(L->exe), "%s", path);
    snprintf(L->cwd, sizeof(L->cwd), "%s", cwd && *cwd ? cwd : "/jarvis/data");
    size_t o = 0;
    for (int i = 0; i < argc && o < sizeof(L->args) - 2; i++) {
        size_t l = strlen(argv[i]);
        if (o + l + 1 >= sizeof(L->args)) break;
        memcpy(L->args + o, argv[i], l + 1);
        o += l + 1;
        L->argc++;
    }
    L->fd[0].type = FD_STDIN;
    L->fd[1].type = FD_STDOUT;
    L->fd[2].type = FD_STDOUT;
    const char *leaf = strrchr(path, '/');
    uint64_t irq;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(irq) :: "memory");
    uint32_t pid = proc_spawn(leaf ? leaf + 1 : path, lx_main, PRIO_USER, 0);
    L->pid = pid;
    if (irq & 0x200) __asm__ volatile ("sti");
    return pid;
}

/* ---- system calls ----------------------------------------------------------------------------- */
static int64_t unsupported(lx_proc_t *L, uint64_t nr)
{
    for (int i = 0; i < L->nwarned; i++) if (L->warned[i] == nr) return -ENOSYS;
    if (L->nwarned < 8) L->warned[L->nwarned++] = (uint32_t)nr;
    terminal_printf("[LINUX] %s: system call %u is not supported yet\n", L->exe, (unsigned)nr);
    return -ENOSYS;
}

static int64_t copy_out(uint64_t dst, const void *src, size_t n)
{
    if (!uok(dst, n, 1)) return -EFAULT;
    memcpy((void *)dst, src, n);
    return 0;
}

int64_t linux_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    lx_proc_t *L = lx_cur();
    if (!L) return -ENOSYS;
    char lp[256], jp[256], jp2[256];
    lx_stat_t st;
    switch (nr) {
    case 0: case 17: {                                      /* read, pread64 */
        if (!uok(a2, a3, 1)) return -EFAULT;
        return fd_read(L, (int)a1, (void *)a2, (size_t)a3, nr == 17 ? (int64_t)a4 : -1);
    }
    case 1: case 18: {                                      /* write, pwrite64 */
        if (!uok(a2, a3, 0)) return -EFAULT;
        return fd_write(L, (int)a1, (const void *)a2, (size_t)a3, nr == 18 ? (int64_t)a4 : -1);
    }
    case 19: case 20: {                                     /* readv, writev */
        if (!uok(a2, a3 * 16, 0)) return -EFAULT;
        int64_t total = 0;
        for (uint64_t i = 0; i < a3; i++) {
            uint64_t base = ((uint64_t *)a2)[i * 2], len = ((uint64_t *)a2)[i * 2 + 1];
            if (!len) continue;
            if (!uok(base, len, nr == 19)) return -EFAULT;
            int64_t r = nr == 19 ? fd_read(L, (int)a1, (void *)base, len, -1) : fd_write(L, (int)a1, (const void *)base, len, -1);
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < len) break;
        }
        return total;
    }
    case 2: return lx_open(L, -100, a1, (int)a2);           /* open */
    case 257: return lx_open(L, (int)a1, a2, (int)a3);      /* openat */
    case 85: return lx_open(L, -100, a1, 0x241);            /* creat */
    case 3:                                                 /* close */
        if (a1 >= LX_FDS || L->fd[a1].type == FD_FREE) return -EBADF;
        fd_close(L, (int)a1);
        return 0;
    case 4: case 6: {                                       /* stat, lstat */
        if (ustr(lp, sizeof(lp), a1)) return -EFAULT;
        const char *t;
        if (nr == 6 && (t = lx_is_link(L, -100, lp, jp2))) stat_link(jp2, t, &st);
        else { int sp = lx_path(L, -100, lp, jp); int64_t r = stat_path(jp, sp, &st); if (r) return r; }
        return copy_out(a2, &st, sizeof(st));
    }
    case 5:                                                 /* fstat */
        { int64_t r = stat_fd(L, (int)a1, &st); if (r) return r; }
        return copy_out(a2, &st, sizeof(st));
    case 262: {                                             /* newfstatat */
        if (ustr(lp, sizeof(lp), a2)) return -EFAULT;
        int64_t r;
        const char *t;
        if (!lp[0] && (a4 & 0x1000)) r = (int)a1 == -100 ? stat_path(L->cwd, 0, &st) : stat_fd(L, (int)a1, &st);
        else if ((a4 & 0x100) && (t = lx_is_link(L, (int)a1, lp, jp2))) { stat_link(jp2, t, &st); r = 0; }
        else { int sp = lx_path(L, (int)a1, lp, jp); r = stat_path(jp, sp, &st); }
        if (r) return r;
        return copy_out(a3, &st, sizeof(st));
    }
    case 8: {                                               /* lseek */
        if (a1 >= LX_FDS) return -EBADF;
        lx_fd_t *f = &L->fd[a1];
        if (f->type == FD_FILE) { int r = vfs_lseek(f->vfd, (int64_t)a2, (int)a3); return r < 0 ? -EINVAL : r; }
        if (f->type == FD_FATFILE) {
            int64_t base = a3 == 1 ? (int64_t)f->off : a3 == 2 ? vfs_size(f->path) : 0;
            if (base + (int64_t)a2 < 0) return -EINVAL;
            f->off = (uint64_t)(base + (int64_t)a2);
            return (int64_t)f->off;
        }
        if (f->type == FD_DIR) { if (a2 == 0 && a3 == 0) { f->dirpos = 0; return 0; } return -EINVAL; }
        return f->type == FD_FREE ? -EBADF : -ESPIPE;
    }
    case 9: return lx_mmap(L, a1, a2, (int)a3, (int)a4, (int)a5, (int64_t)a6);
    case 10:                                                /* mprotect */
        if (a1 & 0xFFF) return -EINVAL;
        vm_region_protect(L->space, a1, (uint32_t)((a2 + 0xFFF) >> 12), prot_flags((int)a3));
        return 0;
    case 11:                                                /* munmap */
        if (a1 & 0xFFF) return -EINVAL;
        vm_region_unmap(L->space, a1, (uint32_t)((a2 + 0xFFF) >> 12));
        return 0;
    case 12:                                                /* brk */
        if (a1 >= L->brk_start && a1 < L->brk_start + (uint64_t)LX_BRK_PAGES * 4096) L->brk_cur = a1;
        return (int64_t)L->brk_cur;
    case 25: return -ENOMEM;                                /* mremap: callers fall back to mmap+copy */
    case 26: case 28: return 0;                             /* msync, madvise */
    case 13:                                                /* rt_sigaction: accepted, never delivered */
        if (a3 && uok(a3, 32, 1)) memset((void *)a3, 0, 32);
        return 0;
    case 14:                                                /* rt_sigprocmask */
        if (a3 && uok(a3, 8, 1)) memset((void *)a3, 0, 8);
        return 0;
    case 131: return 0;                                     /* sigaltstack */
    case 16: {                                              /* ioctl */
        if (a1 >= LX_FDS) return -EBADF;
        int t = L->fd[a1].type;
        if (t != FD_STDIN && t != FD_STDOUT) return -ENOTTY;
        if (a2 == 0x5401) {                                 /* TCGETS: struct termios (kernel layout, 36 bytes) */
            uint8_t tio[36] = { 0 };
            uint32_t iflag = 0x500, oflag = 0x5, cflag = 0xbf, lflag = 0x8a3b;
            memcpy(tio, &iflag, 4); memcpy(tio + 4, &oflag, 4); memcpy(tio + 8, &cflag, 4); memcpy(tio + 12, &lflag, 4);
            static const uint8_t cc[17] = { 3, 0x1c, 0x7f, 0x15, 4, 0, 1, 0, 0x11, 0x13, 0x1a, 0, 0x12, 0x0f, 0x17, 0x16, 0 };
            memcpy(tio + 17, cc, sizeof(cc));               /* c_line at 16, c_cc[19] from 17 */
            return copy_out(a3, tio, sizeof(tio));
        }
        if (a2 == 0x5413) {                                 /* TIOCGWINSZ */
            uint16_t ws[4] = { 30, 100, 0, 0 };
            return copy_out(a3, ws, sizeof(ws));
        }
        if (a2 == 0x540F) { uint32_t pg = kthread_current_pid(); return copy_out(a3, &pg, 4); }   /* TIOCGPGRP */
        if (a2 == 0x5402 || a2 == 0x5403 || a2 == 0x5404 || a2 == 0x5410) return 0;               /* TCSETS*, TIOCSPGRP */
        return -ENOTTY;
    }
    case 21: case 269: case 439: {                          /* access, faccessat, faccessat2 */
        uint64_t up = nr == 21 ? a1 : a2;
        if (ustr(lp, sizeof(lp), up)) return -EFAULT;
        int sp = lx_path(L, nr == 21 ? -100 : (int)a1, lp, jp);
        if (sp) return 0;
        return (vfs_exists(jp) || vfs_is_dir(jp) || !strcmp(jp, LX_ROOT)) ? 0 : -ENOENT;
    }
    case 24: kthread_yield(); return 0;                     /* sched_yield */
    case 32: case 33: case 292: {                           /* dup, dup2, dup3 */
        if (a1 >= LX_FDS || L->fd[a1].type == FD_FREE) return -EBADF;
        int nfd;
        if (nr == 32) { nfd = fd_alloc(L, 0); if (nfd < 0) return nfd; }
        else { nfd = (int)a2; if (nfd < 0 || nfd >= LX_FDS) return -EBADF; if (nfd == (int)a1) return nfd; if (L->fd[nfd].type) fd_close(L, nfd); }
        L->fd[nfd] = L->fd[a1];
        L->fd[nfd].cloexec = nr == 292 && (a3 & 0x80000);
        return nfd;
    }
    case 72: {                                              /* fcntl */
        if (a1 >= LX_FDS || L->fd[a1].type == FD_FREE) return -EBADF;
        switch (a2) {
        case 0: case 1030: { int n = fd_alloc(L, (int)a3); if (n < 0) return n; L->fd[n] = L->fd[a1]; return n; }
        case 1: return L->fd[a1].cloexec;
        case 2: L->fd[a1].cloexec = a3 & 1; return 0;
        case 3: return L->fd[a1].flags & ~0x80000;
        case 4: return 0;
        default: return -EINVAL;
        }
    }
    case 35: case 230: {                                    /* nanosleep, clock_nanosleep */
        uint64_t ts = nr == 35 ? a1 : a3;
        if (!uok(ts, 16, 0)) return -EFAULT;
        uint64_t ticks = ((uint64_t *)ts)[0] * 100 + ((uint64_t *)ts)[1] / 10000000ULL;
        kthread_sleep(ticks ? (uint32_t)ticks : 1);
        return 0;
    }
    case 39: case 186: return kthread_current_pid();        /* getpid, gettid */
    case 110: return 1;                                     /* getppid */
    case 111: case 121: case 124: return kthread_current_pid();   /* getpgrp, getpgid, getsid */
    case 109: case 112: return 0;                           /* setpgid, setsid */
    case 102: case 104: case 107: case 108: return 0;       /* uid/gid: root */
    case 105: case 106: case 113: case 114: case 117: case 119: return 0;   /* setuid ... */
    case 118: case 120:                                     /* getresuid/getresgid */
        if (uok(a1, 4, 1)) *(uint32_t *)a1 = 0;
        if (uok(a2, 4, 1)) *(uint32_t *)a2 = 0;
        if (uok(a3, 4, 1)) *(uint32_t *)a3 = 0;
        return 0;
    case 115: return 0;                                     /* getgroups */
    case 95: return 022;                                    /* umask */
    case 60: case 231:                                      /* exit, exit_group */
        for (int i = 0; i < LX_FDS; i++) if (L->fd[i].type) fd_close(L, i);
        L->pid = 0;
        kthread_exit(a1 & 0xFF);
        return 0;
    case 62:                                                /* kill */
        if (a1 == kthread_current_pid() || a1 == 0) {
            if (a2 == 0) return 0;
            for (int i = 0; i < LX_FDS; i++) if (L->fd[i].type) fd_close(L, i);
            L->pid = 0;
            kthread_exit(128 + (a2 & 0x7F));
        }
        return -EPERM;
    case 200: case 234:                                     /* tkill, tgkill (abort) */
        for (int i = 0; i < LX_FDS; i++) if (L->fd[i].type) fd_close(L, i);
        L->pid = 0;
        kthread_exit(128 + ((nr == 200 ? a2 : a3) & 0x7F));
        return 0;
    case 63: {                                              /* uname */
        char u[6 * 65];
        memset(u, 0, sizeof(u));
        char host[64];
        setting_get("device_name", host, sizeof(host));
        snprintf(u, 65, "Linux");
        snprintf(u + 65, 65, "%s", host[0] ? host : "JARVIS-PC");
        snprintf(u + 130, 65, "6.1.0-jarvis");
        snprintf(u + 195, 65, "#1 SMP JARVIS OS Linux compatibility layer");
        snprintf(u + 260, 65, "x86_64");
        snprintf(u + 325, 65, "(none)");
        return copy_out(a1, u, sizeof(u));
    }
    case 79: {                                              /* getcwd */
        char v[256];
        lx_view(L->cwd, v, sizeof(v));
        size_t l = strlen(v) + 1;
        if (l > a2) return -ERANGE;
        int64_t r = copy_out(a1, v, l);
        return r ? r : (int64_t)l;
    }
    case 80: case 81: {                                     /* chdir, fchdir */
        if (nr == 81) {
            if (a1 >= LX_FDS || L->fd[a1].type != FD_DIR) return -ENOTDIR;
            snprintf(L->cwd, sizeof(L->cwd), "%s", L->fd[a1].path);
            return 0;
        }
        if (ustr(lp, sizeof(lp), a1)) return -EFAULT;
        lx_path(L, -100, lp, jp);
        if (strcmp(jp, LX_ROOT) && !vfs_is_dir(jp)) return -ENOENT;
        snprintf(L->cwd, sizeof(L->cwd), "%s", jp);
        return 0;
    }
    case 217: return lx_getdents64(L, (int)a1, a2, (size_t)a3);
    case 82: case 264: case 316: {                          /* rename, renameat, renameat2 */
        uint64_t f1 = nr == 82 ? a1 : a2, f2 = nr == 82 ? a2 : a4;
        int d1 = nr == 82 ? -100 : (int)a1, d2 = nr == 82 ? -100 : (int)a3;
        if (ustr(lp, sizeof(lp), f1)) return -EFAULT;
        char from_link[256];
        const char *lt = lx_is_link(L, d1, lp, from_link);
        char ltarget[200];
        if (lt) snprintf(ltarget, sizeof(ltarget), "%s", lt);
        lx_path(L, d1, lp, jp);
        if (ustr(lp, sizeof(lp), f2)) return -EFAULT;
        if (lt) {                                           /* renaming a link moves its table entry */
            char to_link[256];
            lx_is_link(L, d2, lp, to_link);
            if (linux_link_remove(from_link)) return -EPERM;
            linux_link_remove(to_link);
            linux_link_set(to_link, ltarget);
            linux_links_save();
            return 0;
        }
        lx_path_f(L, d2, lp, jp2, 0);
        if (vfs_is_private(jp) || vfs_is_private(jp2)) return -EACCES;
        if (vfs_exists(jp2) && !vfs_is_dir(jp2)) vfs_delete(jp2);
        return vfs_rename(jp, jp2) == 0 ? 0 : -ENOENT;
    }
    case 83: case 258:                                      /* mkdir, mkdirat */
        if (ustr(lp, sizeof(lp), nr == 83 ? a1 : a2)) return -EFAULT;
        lx_path(L, nr == 83 ? -100 : (int)a1, lp, jp);
        if (vfs_is_private(jp)) return -EACCES;
        if (vfs_exists(jp) || vfs_is_dir(jp)) return -EEXIST;
        return vfs_mkdir(jp) == 0 ? 0 : -ENOENT;
    case 88: case 266: {                                    /* symlink, symlinkat */
        char target[256];
        if (ustr(target, sizeof(target), a1)) return -EFAULT;
        if (ustr(lp, sizeof(lp), nr == 88 ? a2 : a3)) return -EFAULT;
        if (lx_is_link(L, nr == 88 ? -100 : (int)a2, lp, jp2)) return -EEXIST;
        lx_path_f(L, nr == 88 ? -100 : (int)a2, lp, jp, 0);
        if (vfs_exists(jp) || vfs_is_dir(jp)) return -EEXIST;
        if (strncmp(jp, LX_ROOT, strlen(LX_ROOT)) != 0) return -EPERM; /* JARVIS folders have no links */
        if (linux_link_set(jp2, target)) return -ENOMEM;
        linux_links_save();
        return 0;
    }
    case 84: case 87: case 263: {                           /* rmdir, unlink, unlinkat */
        if (ustr(lp, sizeof(lp), nr == 263 ? a2 : a1)) return -EFAULT;
        if (nr != 84 && !(nr == 263 && (a3 & 0x200)) && lx_is_link(L, nr == 263 ? (int)a1 : -100, lp, jp2)) {
            if (linux_link_remove(jp2)) return -EPERM;              /* /bin and friends */
            linux_links_save();
            return 0;
        }
        lx_path(L, nr == 263 ? (int)a1 : -100, lp, jp);
        if (vfs_is_private(jp)) return -EACCES;
        int want_dir = nr == 84 || (nr == 263 && (a3 & 0x200));
        if (!vfs_exists(jp) && !vfs_is_dir(jp)) return -ENOENT;
        if (want_dir && !vfs_is_dir(jp)) return -ENOTDIR;
        if (!want_dir && vfs_is_dir(jp)) return -EISDIR;
        if (want_dir && vfs_count_children(jp) > 0) return -ENOTEMPTY;
        return vfs_delete(jp) == 0 ? 0 : -ENOENT;
    }
    case 89: case 267: {                                    /* readlink, readlinkat */
        if (ustr(lp, sizeof(lp), nr == 89 ? a1 : a2)) return -EFAULT;
        uint64_t buf = nr == 89 ? a2 : a3, len = nr == 89 ? a3 : a4;
        char v[256];
        const char *t;
        if (!strcmp(lp, "/proc/self/exe")) lx_view(L->exe, v, sizeof(v));
        else if ((t = lx_is_link(L, nr == 89 ? -100 : (int)a1, lp, jp2))) snprintf(v, sizeof(v), "%s", t);
        else { lx_path(L, nr == 89 ? -100 : (int)a1, lp, jp); return (vfs_exists(jp) || vfs_is_dir(jp)) ? -EINVAL : -ENOENT; }
        size_t l = strlen(v) < len ? strlen(v) : len;
        int64_t r = copy_out(buf, v, l);
        return r ? r : (int64_t)l;
    }
    case 90: case 91: case 92: case 93: case 94: case 268: case 260: return 0;   /* chmod, fchmod, chown ... */
    case 76: case 77: {                                     /* truncate, ftruncate */
        const char *p;
        if (nr == 76) { if (ustr(lp, sizeof(lp), a1)) return -EFAULT; lx_path(L, -100, lp, jp); p = jp; }
        else { if (a1 >= LX_FDS || (L->fd[a1].type != FD_FILE && L->fd[a1].type != FD_FATFILE)) return -EBADF; p = L->fd[a1].path; }
        long size = vfs_size(p);
        if (size < 0) return -ENOENT;
        size_t nl = (size_t)a2;
        uint8_t *tmp = (uint8_t *)(nl + 1 >= LARGE_ALLOC_THRESHOLD ? kmalloc_large(nl + 1) : kmalloc(nl + 1));
        if (!tmp) return -ENOMEM;
        memset(tmp, 0, nl + 1);
        if (size > 0) { char *old = (char *)(size + 1 >= (long)LARGE_ALLOC_THRESHOLD ? kmalloc_large((size_t)size + 1) : kmalloc((size_t)size + 1));
            if (old) { vfs_read_file(p, old, (size_t)size + 1); memcpy(tmp, old, (size_t)size < nl ? (size_t)size : nl);
                       if (size + 1 >= (long)LARGE_ALLOC_THRESHOLD) kfree_large(old, (size_t)size + 1); else kfree(old); } }
        int rc = vfs_write_file(p, tmp, nl);
        if (nl + 1 >= LARGE_ALLOC_THRESHOLD) kfree_large(tmp, nl + 1); else kfree(tmp);
        return rc ? -EACCES : 0;
    }
    case 74: case 75: case 162: return 0;                   /* fsync, fdatasync, sync */
    case 96: {                                              /* gettimeofday */
        uint64_t tv[2] = { (uint64_t)sys_unix_time(), (g_kstate.uptime_ticks % 100) * 10000 };
        return a1 ? copy_out(a1, tv, sizeof(tv)) : 0;
    }
    case 201: {                                             /* time */
        uint64_t t = (uint64_t)sys_unix_time();
        if (a1) copy_out(a1, &t, 8);
        return (int64_t)t;
    }
    case 228: case 229: {                                   /* clock_gettime, clock_getres */
        uint64_t ts[2];
        if (nr == 229) { ts[0] = 0; ts[1] = 10000000; }
        else if (a1 == 0 || a1 == 5 || a1 == 8 || a1 == 11) { ts[0] = (uint64_t)sys_unix_time(); ts[1] = (g_kstate.uptime_ticks % 100) * 10000000ULL; }
        else { ts[0] = g_kstate.uptime_ticks / 100; ts[1] = (g_kstate.uptime_ticks % 100) * 10000000ULL; }
        return a2 ? copy_out(a2, ts, sizeof(ts)) : 0;
    }
    case 97: case 302: {                                    /* getrlimit, prlimit64 */
        uint64_t out = nr == 97 ? a2 : a4;
        int res = nr == 97 ? (int)a1 : (int)a2;
        uint64_t lim[2] = { ~0ULL, ~0ULL };
        if (res == 3) { lim[0] = 8 * 1024 * 1024; }         /* RLIMIT_STACK */
        if (res == 7) { lim[0] = LX_FDS; lim[1] = LX_FDS; } /* RLIMIT_NOFILE */
        if (out) return copy_out(out, lim, sizeof(lim));
        return 0;
    }
    case 160: return 0;                                     /* setrlimit */
    case 99: {                                              /* sysinfo */
        uint8_t si[112];
        memset(si, 0, sizeof(si));
        uint64_t total, freep;
        pmm_get_stats(&total, &freep);
        int64_t up = (int64_t)(g_kstate.uptime_ticks / 100);
        memcpy(si, &up, 8);
        uint64_t tr = total * 4096, fr = freep * 4096;
        memcpy(si + 32, &tr, 8);
        memcpy(si + 40, &fr, 8);
        uint16_t procs = 20;
        memcpy(si + 80, &procs, 2);
        uint32_t unit = 1;
        memcpy(si + 104, &unit, 4);
        return copy_out(a1, si, sizeof(si));
    }
    case 137: case 138: {                                   /* statfs, fstatfs */
        uint8_t s[120];
        memset(s, 0, sizeof(s));
        diskfs_stats_t ds;
        uint64_t type = 0xEF53, bsize = 4096, blocks = 0, bfree = 0;
        if (diskfs_get_stats(&ds) == 0) { blocks = ds.total_bytes / 4096; bfree = (ds.total_bytes - ds.used_bytes) / 4096; }
        memcpy(s, &type, 8); memcpy(s + 8, &bsize, 8); memcpy(s + 16, &blocks, 8); memcpy(s + 24, &bfree, 8); memcpy(s + 32, &bfree, 8);
        uint64_t namelen = 255;
        memcpy(s + 64, &namelen, 8);
        return copy_out(a2, s, sizeof(s));
    }
    case 158:                                               /* arch_prctl */
        if (a1 == 0x1002) { proc_set_fs_base(a2); return 0; }             /* ARCH_SET_FS */
        if (a1 == 0x1003) { uint64_t v = proc_get_fs_base(); return copy_out(a2, &v, 8); }
        return -EINVAL;
    case 218: return kthread_current_pid();                 /* set_tid_address */
    case 273: return 0;                                     /* set_robust_list */
    case 334: return -ENOSYS;                               /* rseq: optional */
    case 202: {                                             /* futex (one thread: never blocks) */
        int op = (int)(a2 & 0x7F);
        if (op == 0 || op == 9) { if (uok(a1, 4, 0) && *(uint32_t *)a1 != (uint32_t)a3) return -EAGAIN; return -EAGAIN; }
        return 0;
    }
    case 204: {                                             /* sched_getaffinity */
        uint64_t mask = 1;
        if (a2 < 8) return -EINVAL;
        int64_t r = copy_out(a3, &mask, 8);
        return r ? r : 8;
    }
    case 203: return 0;                                     /* sched_setaffinity */
    case 140: case 141: return 0;                           /* getpriority, setpriority */
    case 318: {                                             /* getrandom */
        if (!uok(a1, a2, 1)) return -EFAULT;
        crypto_random((void *)a1, (size_t)a2);
        return (int64_t)a2;
    }
    case 7: case 271: {                                     /* poll, ppoll: report everything ready */
        if (!uok(a1, a2 * 8, 1)) return -EFAULT;
        for (uint64_t i = 0; i < a2; i++) {
            int16_t *ev = (int16_t *)(a1 + i * 8 + 4), *rev = (int16_t *)(a1 + i * 8 + 6);
            *rev = *ev & 5;
        }
        return (int64_t)a2;
    }
    case 61:                                                /* wait4: no children */
        return -ECHILD;
    case 41: return -EAFNOSUPPORT;                          /* socket */
    case 57: case 58: case 56: case 435: case 59: case 322: /* fork, vfork, clone, clone3, execve, execveat */
        return unsupported(L, nr);
    case 436: return 0;                                     /* close_range */
    case 332: {                                             /* statx(dirfd, path, flags, mask, buf) */
        if (ustr(lp, sizeof(lp), a2)) return -EFAULT;
        int64_t r;
        const char *t;
        if (!lp[0] && (a3 & 0x1000)) r = (int)a1 == -100 ? stat_path(L->cwd, 0, &st) : stat_fd(L, (int)a1, &st);
        else if ((a3 & 0x100) && (t = lx_is_link(L, (int)a1, lp, jp2))) { stat_link(jp2, t, &st); r = 0; }
        else { int sp = lx_path(L, (int)a1, lp, jp); r = stat_path(jp, sp, &st); }
        if (r) return r;
        uint8_t sx[256];
        memset(sx, 0, sizeof(sx));
        uint32_t mask = 0x7FF, blksize = (uint32_t)st.blksize, nlink = (uint32_t)st.nlink;   /* STATX_BASIC_STATS */
        uint16_t mode = (uint16_t)st.mode;
        memcpy(sx + 0, &mask, 4);
        memcpy(sx + 4, &blksize, 4);
        memcpy(sx + 16, &nlink, 4);
        memcpy(sx + 28, &mode, 2);
        memcpy(sx + 32, &st.ino, 8);
        memcpy(sx + 40, &st.size, 8);
        memcpy(sx + 48, &st.blocks, 8);
        int64_t tm = (int64_t)st.mtime;
        memcpy(sx + 64, &tm, 8);                            /* atime */
        memcpy(sx + 96, &tm, 8);                            /* ctime */
        memcpy(sx + 112, &tm, 8);                           /* mtime */
        uint32_t rmaj = (uint32_t)(st.rdev >> 8), rmin = (uint32_t)(st.rdev & 0xFF), dmaj = 8, dmin = 1;
        memcpy(sx + 128, &rmaj, 4); memcpy(sx + 132, &rmin, 4);
        memcpy(sx + 136, &dmaj, 4); memcpy(sx + 140, &dmin, 4);
        return copy_out(a5, sx, sizeof(sx));
    }
    case 221: return 0;                                     /* fadvise64: advice only */
    case 191: case 192: case 193: return -95;               /* getxattr ...: EOPNOTSUPP (no extended attributes) */
    case 194: case 195: case 196: return 0;                 /* listxattr ...: none */
    case 188: case 189: case 190: return -95;               /* setxattr ... */
    case 324: return -ENOSYS;                               /* membarrier */
    case 157: return 0;                                     /* prctl */
    case 22: case 293: return unsupported(L, nr);           /* pipe, pipe2 */
    default:
        return unsupported(L, nr);
    }
    (void)a5; (void)a6;
}

/* The Linux root: folders and the few /etc files programs read. */
void linux_install_root(void)
{
    /* merged /usr: older roots had real /bin, /lib ... folders; move them */
    for (size_t i = 0; i < sizeof(g_usr_alias) / sizeof(g_usr_alias[0]); i++) {
        char from[96], to[96];
        snprintf(from, sizeof(from), "%s/%s", LX_ROOT, g_usr_alias[i]);
        snprintf(to, sizeof(to), "%s/usr/%s", LX_ROOT, g_usr_alias[i]);
        if (!vfs_is_dir(from)) continue;
        if (!vfs_exists(to)) { vfs_rename(from, to); continue; }
        static char listing[16384];
        vfs_list(from, listing, sizeof(listing));
        for (char *p = listing; *p;) {
            char *eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            char *full = strchr(p, '/');
            if (full && full < eol && !strncmp(p, "[file]", 6)) {
                char src[256], dst[256];
                snprintf(src, sizeof(src), "%.*s", (int)(eol - full), full);
                snprintf(dst, sizeof(dst), "%s%s", to, src + strlen(from));
                if (!vfs_exists(dst)) vfs_rename(src, dst);
            }
            p = *eol ? eol + 1 : eol;
        }
        vfs_delete(from);
    }
    static const char *const dirs[] = { "", "/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu",
                                        "/usr/share", "/usr/local", "/usr/local/bin", "/etc", "/etc/apt", "/etc/dpkg", "/etc/dpkg/dpkg.cfg.d",
                                        "/tmp", "/var", "/var/lib", "/var/lib/dpkg", "/var/lib/dpkg/info", "/var/cache", "/var/cache/apt",
                                        "/var/cache/apt/archives", "/var/lib/apt", "/var/lib/apt/lists", "/var/log", "/root", "/home", "/opt",
                                        "/dev", "/proc" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char p[128];
        snprintf(p, sizeof(p), "%s%s", LX_ROOT, dirs[i]);
        if (!vfs_is_dir(p)) vfs_mkdir(p);
    }
    char host[64], hostfile[80];
    setting_get("device_name", host, sizeof(host));
    snprintf(hostfile, sizeof(hostfile), "%s\n", host[0] ? host : "JARVIS-PC");
    struct { const char *path, *text; } files[] = {
        { "/etc/passwd", "root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n" },
        { "/etc/group", "root:x:0:\nnogroup:x:65534:\n" },
        { "/etc/os-release", "PRETTY_NAME=\"JARVIS OS (Linux compatibility, Debian 12 packages)\"\nNAME=\"JARVIS OS\"\nID=jarvis\nID_LIKE=debian\n"
                             "VERSION_ID=\"12\"\nVERSION_CODENAME=bookworm\n" },
        { "/etc/debian_version", "12.0\n" },
        { "/etc/hostname", hostfile },
        { "/etc/hosts", "127.0.0.1 localhost\n" },
        { "/etc/resolv.conf", "nameserver 10.0.2.3\n" },
        { "/etc/nsswitch.conf", "passwd: files\ngroup: files\nhosts: files dns\n" },
        { "/etc/ld.so.conf", "/usr/local/lib\n/usr/lib/x86_64-linux-gnu\n" },
        { "/etc/apt/sources.list", "# Debian packages for the JARVIS OS Linux layer (apt update reads this)\n"
                                   "deb https://deb.debian.org/debian bookworm main\n" },
        /* like Debian's minimal images: no manuals, translations or docs */
        { "/etc/dpkg/dpkg.cfg.d/excludes", "# files dpkg does not unpack (remove a line to keep them)\n"
                                           "path-exclude=/usr/share/man/*\npath-exclude=/usr/share/info/*\n"
                                           "path-exclude=/usr/share/locale/*\npath-exclude=/usr/share/doc/*\n"
                                           "path-include=/usr/share/doc/*/copyright\npath-exclude=/usr/share/lintian/*\n"
                                           "path-exclude=/usr/share/groff/*\npath-exclude=/usr/share/linda/*\n" },
        { "/var/lib/dpkg/status", "" },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char p[128];
        snprintf(p, sizeof(p), "%s%s", LX_ROOT, files[i].path);
        if (!vfs_exists(p)) vfs_write_file(p, files[i].text, strlen(files[i].text));
    }
}
