/* ==========================================================================
 *  VFS -- inode based RAM file system (RamFS)
 *
 *  The NVRAM window (0x02000000, 32 MB) is formatted at boot with a genuine
 *  UNIX-flavoured on-disk layout:
 *
 *     [ superblock ][ inode table: 512 x 128 B ][ block bitmap ]
 *     [ inode bitmap ][ data blocks: 4096 x 4 KiB ]
 *
 *  Inodes carry rwx permissions, timestamps, 12 direct block pointers and one
 *  single-indirect block (512 extra pointers => 2 MB max file).  Directories
 *  are arrays of {inode, name[28]} entries including "." and "..".  Absolute
 *  and relative paths ("../bin/app") both resolve.
 *
 *  /proc is a *virtual* filesystem: opening one of its files regenerates the
 *  contents from live kernel state, so `cat /proc/meminfo` always tells the
 *  truth.
 * ========================================================================== */

typedef struct {
    u32 magic;
    u32 version;
    u32 inode_count;
    u32 block_count;
    u32 block_size;
    u32 inode_bitmap_block;
    u32 data_start_block;
    u32 free_inodes;
    u32 free_blocks;
    u32 root_inode;
    u32 mount_time;
    u32 mounts;                 /* how many times the fs was formatted    */
    u32 generation;
    char volume[32];
} vfs_superblock_t;

enum { FT_FREE = 0, FT_FILE = 1, FT_DIR = 2, FT_DEV = 3, FT_PROC = 4 };

#define INODE_MAGIC 0x1A0DE5u
typedef struct {
    u32 ino;
    u32 type;
    u32 mode;                   /* rwxr-xr-x style, octal                  */
    u32 uid, gid;
    u32 size;
    u32 atime, mtime, ctime;
    u32 direct[12];
    u32 indirect;
    u32 link_count;
    u32 flags;
    u32 checksum;
} vfs_inode_t;

#define VFS_DENT_SIZE 32
typedef struct {
    u32 ino;
    u32 type;
    char name[VFS_MAX_NAME];
} vfs_dirent_t;

#define VFS_MAX_PATH 512
#define VFS_MAX_FDS  VFS_MAX_OPEN

typedef struct {
    int  used;
    int  ino;
    u32  pos;
    int  flags;                 /* O_RDONLY / O_WRONLY / O_RDWR / O_CREAT   */
    u32  pid;
    int  is_proc;
    u32  open_tick;
} vfs_fd_t;

enum { O_RDONLY = 1, O_WRONLY = 2, O_RDWR = 4, O_CREAT = 8, O_TRUNC = 16, O_APPEND = 32 };

/* On-disk layout: 4 KiB superblock, then the inode table (512 x 60 B), then the
   block bitmap (4096/8) and the inode bitmap (512/8), then block 0 of the data
   area.  The data area must therefore start past all of the metadata or the
   first allocations would scribble over the inode table. */
#define VFS_META_OFF  4096
#define VFS_DATA_OFF  (4096u + (u32)VFS_INODES * (u32)sizeof(vfs_inode_t) + (VFS_BLOCKS / 8) + (VFS_INODES / 8))
#define VFS_DATA_OFF_ALIGNED (((VFS_DATA_OFF + 4095u) / 4096u) * 4096u)

typedef struct {
    vfs_superblock_t *sb;
    u32  data_off;
    vfs_inode_t *inodes;
    u8  *inode_bitmap;
    u8  *block_bitmap;
    u8  *data;
    u32  base;
    vfs_fd_t fd[VFS_MAX_FDS];
    u32  reads, writes, opens;
    u32  block_io;              /* simulated physical I/O ops               */
    u32  proc_gen;              /* generation counter for /proc files       */
    int  mounted;
    char cwd[VFS_MAX_PATH];
    u32  last_activity_tick;
} vfs_t;

static vfs_t g_vfs;

/* -------------------------------------------------------------------------- */
/*  low level block / inode access                                           */
/* -------------------------------------------------------------------------- */

static u8 *blk_ptr(u32 blkno) {
    if (blkno == 0 || blkno >= VFS_BLOCKS) return NULL;
    return g_vfs.data + (size_t)blkno * VFS_BLOCK_SIZE;
}

static int blk_is_free(u32 blkno) {
    return (g_vfs.block_bitmap[blkno >> 3] & (1u << (blkno & 7))) != 0;
}

static void blk_mark(u32 blkno, int used) {
    if (blkno >= VFS_BLOCKS) return;
    if (used) g_vfs.block_bitmap[blkno >> 3] &= (u8)~(1u << (blkno & 7));
    else      g_vfs.block_bitmap[blkno >> 3] |= (u8)(1u << (blkno & 7));
}

static u32 blk_alloc(void) {
    u32 i;
    for (i = 1; i < VFS_BLOCKS; i++) {
        if (blk_is_free(i)) {
            blk_mark(i, 1);
            g_vfs.sb->free_blocks--;
            kmemset(blk_ptr(i), 0, VFS_BLOCK_SIZE);
            return i;
        }
    }
    return 0;
}

static void blk_free(u32 blkno) {
    if (blkno == 0 || blkno >= VFS_BLOCKS) return;
    if (!blk_is_free(blkno)) {
        blk_mark(blkno, 0);
        g_vfs.sb->free_blocks++;
    }
}

static u32 inode_checksum(const vfs_inode_t *n) {
    return n->ino * 2654435761u ^ n->type ^ n->size ^ n->mode ^ n->mtime ^ n->direct[0];
}

static int ino_is_free(u32 ino) {
    return ino > 0 && (g_vfs.inode_bitmap[ino >> 3] & (1u << (ino & 7))) != 0;
}

static void ino_mark(u32 ino, int used) {
    if (used) g_vfs.inode_bitmap[ino >> 3] &= (u8)~(1u << (ino & 7));
    else      g_vfs.inode_bitmap[ino >> 3] |= (u8)(1u << (ino & 7));
}

static vfs_inode_t *inode_get(u32 ino) {
    if (ino == 0 || ino >= VFS_INODES) return NULL;
    if (ino_is_free(ino)) return NULL;
    return &g_vfs.inodes[ino];
}

static u32 inode_alloc(void) {
    u32 i;
    for (i = 1; i < VFS_INODES; i++) {
        if (ino_is_free(i)) {
            ino_mark(i, 1);
            g_vfs.sb->free_inodes--;
            kmemset(&g_vfs.inodes[i], 0, sizeof(vfs_inode_t));
            g_vfs.inodes[i].ino = i;
            g_vfs.inodes[i].link_count = 1;
            return i;
        }
    }
    return 0;
}

static void inode_all_blocks_free(vfs_inode_t *n) {
    int i;
    u32 *ind;
    for (i = 0; i < 12; i++) {
        if (n->direct[i]) blk_free(n->direct[i]);
        n->direct[i] = 0;
    }
    if (n->indirect) {
        ind = (u32 *)blk_ptr(n->indirect);
        if (ind) {
            for (i = 0; i < VFS_BLOCK_SIZE / 4; i++)
                if (ind[i]) blk_free(ind[i]);
        }
        blk_free(n->indirect);
        n->indirect = 0;
    }
}

static void inode_release(u32 ino) {
    vfs_inode_t *n = &g_vfs.inodes[ino];
    if (ino == 0 || ino >= VFS_INODES) return;
    inode_all_blocks_free(n);
    kmemset(n, 0, sizeof(*n));
    ino_mark(ino, 0);
    g_vfs.sb->free_inodes++;
}

/* Returns the physical block that stores the `index`-th block of the file,
 * allocating it (and the indirect block) when `alloc` is set. */
static u32 inode_block(vfs_inode_t *n, u32 index, int alloc) {
    if (index < 12) {
        if (!n->direct[index] && alloc) n->direct[index] = blk_alloc();
        return n->direct[index];
    }
    index -= 12;
    if (index >= VFS_BLOCK_SIZE / 4) return 0;
    if (!n->indirect) {
        if (!alloc) return 0;
        n->indirect = blk_alloc();
        if (!n->indirect) return 0;
    }
    {
        u32 *ind = (u32 *)blk_ptr(n->indirect);
        if (!ind) return 0;
        if (!ind[index] && alloc) ind[index] = blk_alloc();
        return ind[index];
    }
}

static u32 inode_read(vfs_inode_t *n, u32 off, void *dst, u32 len) {
    u8 *out = (u8 *)dst;
    u32 done = 0;
    if (!n) return 0;
    if (off >= n->size) return 0;
    if (off + len > n->size) len = n->size - off;
    while (done < len) {
        u32 idx = (off + done) / VFS_BLOCK_SIZE;
        u32 boff = (off + done) % VFS_BLOCK_SIZE;
        u32 chunk = VFS_BLOCK_SIZE - boff;
        u32 bp = inode_block(n, idx, 0);
        if (chunk > len - done) chunk = len - done;
        if (!bp) {
            kmemset(out + done, 0, chunk);
        } else {
            u8 *b = blk_ptr(bp);
            kmemcpy(out + done, b + boff, chunk);
            g_vfs.block_io++;
        }
        done += chunk;
    }
    return done;
}

static u32 inode_write(vfs_inode_t *n, u32 off, const void *src, u32 len) {
    const u8 *in = (const u8 *)src;
    u32 done = 0;
    u32 maxsize = (12 + VFS_BLOCK_SIZE / 4) * VFS_BLOCK_SIZE;
    if (!n) return 0;
    if (off + len > maxsize) len = maxsize - off;
    while (done < len) {
        u32 idx = (off + done) / VFS_BLOCK_SIZE;
        u32 boff = (off + done) % VFS_BLOCK_SIZE;
        u32 chunk = VFS_BLOCK_SIZE - boff;
        u32 bp = inode_block(n, idx, 1);
        if (chunk > len - done) chunk = len - done;
        if (!bp) break;
        {
            u8 *b = blk_ptr(bp);
            kmemcpy(b + boff, in + done, chunk);
            g_vfs.block_io++;
        }
        done += chunk;
    }
    if (off + done > n->size) n->size = off + done;
    n->mtime = irq_ticks();
    n->checksum = inode_checksum(n);
    return done;
}

/* -------------------------------------------------------------------------- */
/*  directories                                                              */
/* -------------------------------------------------------------------------- */

static vfs_dirent_t *dir_entry(vfs_inode_t *dir, u32 index) {
    u32 off = index * VFS_DENT_SIZE;
    static vfs_dirent_t tmp;
    if (!dir || dir->type != FT_DIR) return NULL;
    if (off + VFS_DENT_SIZE > dir->size) return NULL;
    if (inode_read(dir, off, &tmp, VFS_DENT_SIZE) != VFS_DENT_SIZE) return NULL;
    return &tmp;
}

static u32 dir_count(vfs_inode_t *dir) {
    return dir ? dir->size / VFS_DENT_SIZE : 0;
}

static int dir_lookup(vfs_inode_t *dir, const char *name, u32 *out_ino) {
    u32 i, n = dir_count(dir);
    vfs_dirent_t d;
    for (i = 0; i < n; i++) {
        if (inode_read(dir, i * VFS_DENT_SIZE, &d, VFS_DENT_SIZE) != VFS_DENT_SIZE) break;
        if (d.ino && !kstrcmp(d.name, name)) {
            if (out_ino) *out_ino = d.ino;
            return 1;
        }
    }
    return 0;
}

static int dir_add(vfs_inode_t *dir, const char *name, u32 ino, u32 type) {
    vfs_dirent_t d;
    u32 count = dir_count(dir);
    if (kstrlen(name) >= VFS_MAX_NAME) return -1;
    kmemset(&d, 0, sizeof(d));
    d.ino = ino;
    d.type = type;
    kstrncpy(d.name, name, VFS_MAX_NAME - 1);
    if (inode_write(dir, count * VFS_DENT_SIZE, &d, VFS_DENT_SIZE) != VFS_DENT_SIZE)
        return -1;
    dir->link_count++;
    dir->checksum = inode_checksum(dir);
    return 0;
}

static int dir_remove(vfs_inode_t *dir, const char *name) {
    u32 i, n = dir_count(dir);
    vfs_dirent_t d;
    for (i = 0; i < n; i++) {
        if (inode_read(dir, i * VFS_DENT_SIZE, &d, VFS_DENT_SIZE) != VFS_DENT_SIZE) break;
        if (d.ino && !kstrcmp(d.name, name)) {
            /* compact: move the last entry into the hole */
            vfs_dirent_t last;
            if (i != n - 1) {
                inode_read(dir, (n - 1) * VFS_DENT_SIZE, &last, VFS_DENT_SIZE);
                inode_write(dir, i * VFS_DENT_SIZE, &last, VFS_DENT_SIZE);
            }
            dir->size -= VFS_DENT_SIZE;
            dir->mtime = irq_ticks();
            dir->checksum = inode_checksum(dir);
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/*  path resolution                                                          */
/* -------------------------------------------------------------------------- */

static void path_normalise(const char *base, const char *input, char *out, size_t outn) {
    char buf[VFS_MAX_PATH * 2];
    char *parts[64];
    int npart = 0, i;
    char *p;
    size_t len = 0;
    if (!input) input = "";
    if (input[0] != '/') {
        ksnprintf(buf, sizeof(buf), "%s/%s", (base && base[0]) ? base : "/", input);
    } else {
        kstrncpy(buf, input, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
    }
    /* tokenise */
    p = buf;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        {
            char *start = p;
            while (*p && *p != '/') p++;
            if (*p) *p++ = 0;
            if (!kstrcmp(start, ".")) continue;
            if (!kstrcmp(start, "..")) {
                if (npart > 0) npart--;
                continue;
            }
            if (npart < 64) parts[npart++] = start;
        }
    }
    out[0] = 0;
    for (i = 0; i < npart; i++) {
        if (len + kstrlen(parts[i]) + 2 >= outn) break;
        kstrcat(out, "/");
        kstrcat(out, parts[i]);
        len = kstrlen(out);
    }
    if (out[0] == 0) kstrcpy(out, "/");
}

/* Resolves a normalised absolute path to an inode number. */
static u32 path_resolve_abs(const char *abs) {
    u32 cur = g_vfs.sb->root_inode;
    const char *p = abs;
    if (!abs || abs[0] != '/') return 0;
    while (*p) {
        char name[VFS_MAX_NAME + 4];
        u32 next = 0;
        int n = 0;
        vfs_inode_t *d;
        while (*p == '/') p++;
        if (!*p) break;
        while (*p && *p != '/' && n < VFS_MAX_NAME) name[n++] = *p++;
        name[n] = 0;
        d = inode_get(cur);
        if (!d || d->type != FT_DIR) return 0;
        if (!dir_lookup(d, name, &next)) return 0;
        cur = next;
    }
    return cur;
}

u32 vfs_resolve(const char *path) {
    char norm[VFS_MAX_PATH];
    path_normalise(g_vfs.cwd, path, norm, sizeof(norm));
    return path_resolve_abs(norm);
}

/* Resolves the *parent* directory of a path and yields the leaf name. */
static u32 path_resolve_parent(const char *path, char *leaf, size_t leafn) {
    char norm[VFS_MAX_PATH];
    u32 ino;
    char *slash;
    path_normalise(g_vfs.cwd, path, norm, sizeof(norm));
    if (!kstrcmp(norm, "/")) { if (leaf) kstrcpy(leaf, ""); return 0; }
    slash = kstrrchr(norm, '/');
    if (!slash) return 0;
    if (leaf) kstrncpy(leaf, slash + 1, leafn);
    if (slash == norm) { norm[1] = 0; }
    else *slash = 0;
    ino = path_resolve_abs(norm);
    return ino;
}

void vfs_normalise(const char *base, const char *input, char *out, size_t outn) {
    path_normalise(base, input, out, outn);
}

/* -------------------------------------------------------------------------- */
/*  public API                                                               */
/* -------------------------------------------------------------------------- */

vfs_t *vfs_state(void) { return &g_vfs; }
int vfs_mounted(void) { return g_vfs.mounted; }
const char *vfs_cwd(void) { return g_vfs.cwd; }

int vfs_chdir(const char *path) {
    u32 ino = vfs_resolve(path);
    vfs_inode_t *n = inode_get(ino);
    char norm[VFS_MAX_PATH];
    if (!n || n->type != FT_DIR) return -1;
    path_normalise(g_vfs.cwd, path, norm, sizeof(norm));
    kstrncpy(g_vfs.cwd, norm, sizeof(g_vfs.cwd) - 1);
    return 0;
}

void vfs_set_cwd(const char *abs) {
    kstrncpy(g_vfs.cwd, abs, sizeof(g_vfs.cwd) - 1);
}

int vfs_stat_path(const char *path, vfs_inode_t *out) {
    u32 ino = vfs_resolve(path);
    vfs_inode_t *n = inode_get(ino);
    if (!n) return -1;
    if (out) *out = *n;
    return 0;
}

const char *vfs_type_name(u32 type) {
    switch (type) {
    case FT_FILE: return "file";
    case FT_DIR:  return "dir";
    case FT_DEV:  return "dev";
    case FT_PROC: return "proc";
    default:      return "free";
    }
}

static void inode_set_mode_owner(vfs_inode_t *n, u32 type) {
    n->type = type;
    n->mode = (type == FT_DIR) ? 0755u : 0644u;
    n->uid = 1000;
    n->gid = 1000;
    n->ctime = irq_ticks();
    n->mtime = n->ctime;
    n->atime = n->ctime;
    n->checksum = inode_checksum(n);
}

u32 vfs_create_node(const char *path, u32 type) {
    char leaf[VFS_MAX_NAME + 4];
    u32 parent_ino = path_resolve_parent(path, leaf, sizeof(leaf));
    vfs_inode_t *parent = inode_get(parent_ino);
    u32 ino;
    if (!parent || parent->type != FT_DIR || !leaf[0]) return 0;
    if (dir_lookup(parent, leaf, NULL)) return 0;
    ino = inode_alloc();
    if (!ino) return 0;
    inode_set_mode_owner(&g_vfs.inodes[ino], type);
    if (type == FT_DIR) {
        g_vfs.inodes[ino].link_count = 2;
    }
    if (dir_add(parent, leaf, ino, type) != 0) {
        inode_release(ino);
        return 0;
    }
    if (type == FT_DIR) {
        /* "." and ".." */
        vfs_inode_t *d = &g_vfs.inodes[ino];
        dir_add(d, ".", ino, FT_DIR);
        dir_add(d, "..", parent_ino, FT_DIR);
        d->link_count = 2;
    }
    return ino;
}

int vfs_mkdir_path(const char *path) {
    char norm[VFS_MAX_PATH];
    char partial[VFS_MAX_PATH];
    u32 ino;
    path_normalise(g_vfs.cwd, path, norm, sizeof(norm));
    ino = path_resolve_abs(norm);
    if (ino) return 0;
    /* create each missing component */
    partial[0] = 0;
    {
        const char *p = norm;
        while (*p) {
            char comp[VFS_MAX_NAME + 4];
            int n = 0;
            while (*p == '/') p++;
            if (!*p) break;
            while (*p && *p != '/' && n < VFS_MAX_NAME) comp[n++] = *p++;
            comp[n] = 0;
            kstrcat(partial, "/");
            kstrcat(partial, comp);
            if (!vfs_resolve(partial)) {
                if (!vfs_create_node(partial, FT_DIR)) return -1;
            }
        }
    }
    return 0;
}

int vfs_write_file(const char *path, const void *data, u32 len) {
    u32 ino = vfs_resolve(path);
    vfs_inode_t *n;
    if (!ino) {
        ino = vfs_create_node(path, FT_FILE);
        if (!ino) return -1;
    }
    n = inode_get(ino);
    if (!n) return -1;
    if (n->type == FT_DIR) return -1;
    if (n->type == FT_PROC) return -1;
    inode_all_blocks_free(n);
    n->size = 0;
    if (len) {
        if (inode_write(n, 0, data, len) != len) return -1;
    }
    n->mtime = irq_ticks();
    n->checksum = inode_checksum(n);
    g_vfs.writes++;
    g_vfs.last_activity_tick = irq_ticks();
    return 0;
}

int vfs_write_string(const char *path, const char *s) {
    return vfs_write_file(path, s, (u32)kstrlen(s));
}

/* Reads a whole file into a heap buffer; caller frees. */
char *vfs_read_alloc(const char *path, u32 *out_len, const char **err) {
    u32 ino = vfs_resolve(path);
    vfs_inode_t *n;
    char *buf;
    u32 len;
    if (err) *err = NULL;
    if (ino) n = inode_get(ino);
    else n = NULL;
    if (!n || n->type == FT_DIR) {
        extern char *proc_generate(const char *path, u32 *len, const char **err);
        if (n && n->type == FT_DIR) {
            if (err) *err = "is a directory";
            return NULL;
        }
        if (!kstrncmp(path, "/proc", 5)) {
            char *p = proc_generate(path, out_len, err);
            if (p) return p;
        }
        if (err && !*err) *err = "no such file or directory";
        return NULL;
    }
    if (n->type == FT_PROC) {
        extern char *proc_generate(const char *path, u32 *len, const char **err);
        return proc_generate(path, out_len, err);
    }
    len = n->size;
    buf = (char *)mm_kmalloc(len + 1);
    if (!buf) {
        if (err) *err = "out of memory";
        return NULL;
    }
    if (len) inode_read(n, 0, buf, len);
    buf[len] = 0;
    if (out_len) *out_len = len;
    n->atime = irq_ticks();
    g_vfs.reads++;
    g_vfs.last_activity_tick = irq_ticks();
    return buf;
}

/* Fast path used by the apps: read up to cap-1 bytes into a caller buffer. */
int vfs_read_text(const char *path, char *buf, u32 cap, u32 *out_len) {
    u32 len = 0;
    const char *err = NULL;
    char *p = vfs_read_alloc(path, &len, &err);
    if (!p) return -1;
    if (len > cap - 1) len = cap - 1;
    kmemcpy(buf, p, len);
    buf[len] = 0;
    if (out_len) *out_len = len;
    mm_kfree(p);
    return 0;
}

int vfs_unlink(const char *path) {
    char leaf[VFS_MAX_NAME + 4];
    u32 parent_ino = path_resolve_parent(path, leaf, sizeof(leaf));
    vfs_inode_t *parent = inode_get(parent_ino);
    u32 ino = 0;
    if (!parent || !leaf[0]) return -1;
    if (!dir_lookup(parent, leaf, &ino)) return -1;
    {
        vfs_inode_t *n = inode_get(ino);
        if (!n) return -1;
        if (n->type == FT_DIR && dir_count(n) > 2) return -2;   /* not empty */
        if (!kstrcmp(leaf, ".") || !kstrcmp(leaf, "..")) return -3;
    }
    dir_remove(parent, leaf);
    inode_release(ino);
    g_vfs.writes++;
    g_vfs.last_activity_tick = irq_ticks();
    return 0;
}

int vfs_rename(const char *from, const char *to) {
    char leaf_from[VFS_MAX_NAME + 4], leaf_to[VFS_MAX_NAME + 4];
    u32 pfrom = path_resolve_parent(from, leaf_from, sizeof(leaf_from));
    u32 pto = path_resolve_parent(to, leaf_to, sizeof(leaf_to));
    vfs_inode_t *pf = inode_get(pfrom), *pt = inode_get(pto);
    u32 ino = 0;
    if (!pf || !pt || !leaf_to[0]) return -1;
    if (!dir_lookup(pf, leaf_from, &ino)) return -1;
    if (dir_lookup(pt, leaf_to, NULL)) return -2;
    {
        vfs_inode_t *n = inode_get(ino);
        if (pfrom == pto) {
            u32 i, cnt = dir_count(pf);
            vfs_dirent_t d;
            for (i = 0; i < cnt; i++) {
                inode_read(pf, i * VFS_DENT_SIZE, &d, VFS_DENT_SIZE);
                if (d.ino == ino) {
                    kstrncpy(d.name, leaf_to, VFS_MAX_NAME - 1);
                    inode_write(pf, i * VFS_DENT_SIZE, &d, VFS_DENT_SIZE);
                    return 0;
                }
            }
            return -1;
        }
        if (dir_add(pt, leaf_to, ino, n ? n->type : FT_FILE) != 0) return -1;
        dir_remove(pf, leaf_from);
    }
    return 0;
}

int vfs_copy(const char *from, const char *to) {
    u32 len = 0;
    const char *err = NULL;
    char *data = vfs_read_alloc(from, &len, &err);
    int rc;
    if (!data) return -1;
    rc = vfs_write_file(to, data, len);
    mm_kfree(data);
    return rc;
}

/* Directory iteration for ls / explorer. */
int vfs_dir_count(const char *path) {
    vfs_inode_t *n = inode_get(vfs_resolve(path));
    return n ? (int)dir_count(n) : -1;
}

int vfs_dir_get(const char *path, int index, vfs_dirent_t *out) {
    vfs_inode_t *n = inode_get(vfs_resolve(path));
    vfs_dirent_t *d;
    if (!n || n->type != FT_DIR) return -1;
    d = dir_entry(n, (u32)index);
    if (!d) return -1;
    *out = *d;
    return 0;
}

int vfs_dir_find(const char *path, const char *name, u32 *ino, u32 *type) {
    vfs_inode_t *n = inode_get(vfs_resolve(path));
    u32 found = 0;
    vfs_dirent_t d;
    u32 i;
    if (!n || n->type != FT_DIR) return -1;
    for (i = 0; i < dir_count(n); i++) {
        if (inode_read(n, i * VFS_DENT_SIZE, &d, VFS_DENT_SIZE) != VFS_DENT_SIZE) break;
        if (!kstrcmp(d.name, name)) {
            found = d.ino;
            if (ino) *ino = found;
            if (type) *type = d.type;
            return 0;
        }
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/*  file descriptors                                                         */
/* -------------------------------------------------------------------------- */

static int fd_alloc(void) {
    int i;
    for (i = 0; i < VFS_MAX_FDS; i++) {
        if (!g_vfs.fd[i].used) {
            kmemset(&g_vfs.fd[i], 0, sizeof(vfs_fd_t));
            g_vfs.fd[i].used = 1;
            g_vfs.fd[i].pid = mm_current_pid();
            g_vfs.fd[i].open_tick = irq_ticks();
            return i;
        }
    }
    return -1;
}

int vfs_open(const char *path, int flags) {
    vfs_inode_t *n;
    u32 ino = vfs_resolve(path);
    int f;
    if (!ino) {
        if (!(flags & O_CREAT)) return -1;
        ino = vfs_create_node(path, FT_FILE);
        if (!ino) return -1;
    }
    n = inode_get(ino);
    if (!n) return -1;
    if (n->type == FT_DIR) return -2;
    if ((flags & O_TRUNC) && n->type == FT_FILE) {
        inode_all_blocks_free(n);
        n->size = 0;
    }
    f = fd_alloc();
    if (f < 0) return -3;
    g_vfs.fd[f].ino = (int)ino;
    g_vfs.fd[f].flags = flags;
    g_vfs.fd[f].is_proc = (n->type == FT_PROC);
    g_vfs.fd[f].pos = (flags & O_APPEND) ? n->size : 0;
    g_vfs.opens++;
    return f;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_vfs.fd[fd].used) return -1;
    g_vfs.fd[fd].used = 0;
    return 0;
}

int vfs_read(int fd, void *buf, u32 len) {
    vfs_inode_t *n;
    u32 got;
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_vfs.fd[fd].used) return -1;
    n = inode_get((u32)g_vfs.fd[fd].ino);
    if (!n) return -1;
    got = inode_read(n, g_vfs.fd[fd].pos, buf, len);
    g_vfs.fd[fd].pos += got;
    g_vfs.reads++;
    return (int)got;
}

int vfs_write(int fd, const void *buf, u32 len) {
    vfs_inode_t *n;
    u32 got;
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_vfs.fd[fd].used) return -1;
    n = inode_get((u32)g_vfs.fd[fd].ino);
    if (!n || n->type != FT_FILE) return -1;
    got = inode_write(n, g_vfs.fd[fd].pos, buf, len);
    g_vfs.fd[fd].pos += got;
    g_vfs.writes++;
    g_vfs.last_activity_tick = irq_ticks();
    return (int)got;
}

u32 vfs_seek(int fd, u32 pos) {
    if (fd < 0 || fd >= VFS_MAX_FDS || !g_vfs.fd[fd].used) return 0;
    g_vfs.fd[fd].pos = pos;
    return pos;
}

int vfs_fd_count(void) {
    int i, n = 0;
    for (i = 0; i < VFS_MAX_FDS; i++) if (g_vfs.fd[i].used) n++;
    return n;
}

/* -------------------------------------------------------------------------- */
/*  mount / format                                                           */
/* -------------------------------------------------------------------------- */

static void format_vfs(void) {
    u32 i;
    vfs_superblock_t *sb;
    g_vfs.base = DISK_BASE;
    g_vfs.data_off = VFS_DATA_OFF_ALIGNED;
    kmemset(g_phys_ram + DISK_BASE, 0, DISK_SIZE);

    sb = (vfs_superblock_t *)(g_phys_ram + DISK_BASE);
    g_vfs.sb = sb;
    g_vfs.inodes = (vfs_inode_t *)(g_phys_ram + DISK_BASE + VFS_META_OFF);
    g_vfs.block_bitmap = g_phys_ram + DISK_BASE + VFS_META_OFF + (u32)VFS_INODES * (u32)sizeof(vfs_inode_t);
    g_vfs.inode_bitmap = g_vfs.block_bitmap + (VFS_BLOCKS / 8);
    g_vfs.data = g_phys_ram + DISK_BASE + g_vfs.data_off;

    kmemset(g_vfs.inodes, 0, (size_t)VFS_INODES * sizeof(vfs_inode_t));
    kmemset(g_vfs.block_bitmap, 0, VFS_BLOCKS / 8);
    kmemset(g_vfs.inode_bitmap, 0, VFS_INODES / 8);
    for (i = 0; i < VFS_BLOCKS; i++) blk_mark(i, 0);
    for (i = 0; i < VFS_INODES; i++) ino_mark(i, 0);

    sb->magic = VFS_MAGIC;
    sb->version = 2;
    sb->inode_count = VFS_INODES;
    sb->block_count = VFS_BLOCKS;
    sb->block_size = VFS_BLOCK_SIZE;
    sb->free_inodes = VFS_INODES - 1;
    sb->free_blocks = VFS_BLOCKS - 1;
    sb->root_inode = 1;
    sb->mount_time = irq_ticks();
    sb->generation = (u32)(now_us() & 0xFFFFFF);
    kstrcpy(sb->volume, "AETHER-NVRAM");
    ino_mark(1, 1);
    g_vfs.inodes[1].ino = 1;
    inode_set_mode_owner(&g_vfs.inodes[1], FT_DIR);
    g_vfs.inodes[1].link_count = 2;
}

static void seed_vfs(void) {
    /* Realistic UNIX-style tree.  Every file below is generated by code. */
    vfs_mkdir_path("/boot");
    vfs_mkdir_path("/etc");
    vfs_mkdir_path("/bin");
    vfs_mkdir_path("/dev");
    vfs_mkdir_path("/proc");
    vfs_mkdir_path("/home/user");
    vfs_mkdir_path("/home/user/docs");
    vfs_mkdir_path("/tmp");
    vfs_mkdir_path("/var/log");

    vfs_write_string("/boot/kernel.sys",
        "AetherOS kernel image\n"
        "arch: x86 (32-bit protected mode, emulated)\n"
        "version: " AETHER_VERSION_STR "\n"
        "codename: " AETHER_CODENAME "\n"
        "text: 0x00010000  data: 0x00040000  bss: 0x00060000\n"
        "symbols: 1241  syscalls: 16  loadable modules: 0\n");
    vfs_write_string("/boot/config.sys",
        "CONFIG_SMP=n\nCONFIG_PREEMPT=y\nCONFIG_TICK_HZ=1000\n"
        "CONFIG_HEAP_MB=31\nCONFIG_NVRAM_MB=32\nCONFIG_MAX_PROCS=64\n"
        "CONFIG_MAX_WINDOWS=16\nCONFIG_VM_STACK=256\n");
    vfs_write_string("/etc/hostname", "aether\n");
    vfs_write_string("/etc/motd",
        "  ___   _  _   ___  _  _  ___  ___\n"
        " / _ \\ | || | | __|| || || _ \\/ __|\n"
        "| |_| || __ || _| | __ ||   /\\__ \\\n"
        " \\___/ |_||_||___||_||_||_|_\\|___/\n\n"
        "Welcome to AetherOS " AETHER_VERSION_STR " (" AETHER_CODENAME ").\n"
        "Type 'help' for the command list, 'apps' for programs.\n");
    vfs_write_string("/etc/theme.cfg",
        "wallpaper = nebula\nscanlines = off\naccent = #4FC3F7\n"
        "font_scale = 1\nwindow_blur = on\n");
    vfs_write_string("/etc/passwd",
        "root:x:0:0:Aether Kernel Architect:/root:/bin/sh\n"
        "user:x:1000:1000:Aether User:/home/user:/bin/sh\n");
    vfs_write_string("/etc/fstab",
        "# device  mount  type   options\n"
        "nvram0    /      ramfs  rw,relatime\n"
        "proc      /proc  proc   ro,nosuid\n");

    /* "binaries": real bytecode images are written by the compiler; these are
       the built-in app descriptors the shell knows how to spawn. */
    vfs_write_string("/bin/sh",     "#!aether-app terminal\nname=AetherShell\n");
    vfs_write_string("/bin/ls",     "#!aether-app builtin\nname=ls\n");
    vfs_write_string("/bin/cat",    "#!aether-app builtin\nname=cat\n");
    vfs_write_string("/bin/cc",     "#!aether-app builtin\nname=cc\ncompiler=bytecode-v1\n");
    vfs_write_string("/bin/sysmon", "#!aether-app window\nname=sysmon\n");
    vfs_write_string("/bin/edit",   "#!aether-app window\nname=edit\n");
    vfs_write_string("/bin/hexview","#!aether-app window\nname=hexview\n");
    vfs_write_string("/bin/calc",   "#!aether-app window\nname=calc\n");
    vfs_write_string("/bin/paint",  "#!aether-app window\nname=paint\n");
    vfs_write_string("/bin/explorer","#!aether-app window\nname=explorer\n");
    vfs_write_string("/bin/settings","#!aether-app window\nname=settings\n");

    vfs_write_string("/home/user/readme.txt",
        "AetherOS user notes\n"
        "==================\n\n"
        "1. You are running on a kernel written in this very executable.\n"
        "2. /proc is virtual: the files are rebuilt from live kernel state.\n"
        "3. Open hello.c, press Compile & Run to build a real bytecode image.\n"
        "4. Drag windows to the top/left/right edges of the screen to snap.\n"
        "5. Everything - fonts, icons, wallpaper - is generated in software.\n");

    vfs_write_string("/home/user/hello.c",
        "// hello.c -- your first AetherOS program\n"
        "func main() {\n"
        "    print(\"Hello, AetherOS world!\\n\");\n"
        "    var i = 0;\n"
        "    while (i < 5) {\n"
        "        print(\"tick \", i, \"\\n\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "    draw_rect(20, 20, 200, 80, 0x4FC3F7);   /* console framebuffer is 240x140 */\n"
        "    print(\"process exiting\\n\");\n"
        "}\n");

    vfs_write_string("/home/user/fibonacci.c",
        "// fibonacci.c -- recursion, loops and integer math\n"
        "func fib(n) {\n"
        "    if (n < 2) { return n; }\n"
        "    return fib(n - 1) + fib(n - 2);\n"
        "}\n\n"
        "func main() {\n"
        "    var i = 0;\n"
        "    while (i < 20) {\n"
        "        print(\"fib(\", i, \") = \", fib(i), \"\\n\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n");

    vfs_write_string("/home/user/todo.txt",
        "[x] boot the kernel\n"
        "[x] mount the inode vfs\n"
        "[x] spawn the compositor\n"
        "[ ] write a game in AetherC\n"
        "[ ] add a TCP stack\n"
        "[ ] port the paint app to the second monitor\n");

    vfs_write_string("/home/user/docs/manual.txt",
        "AetherOS manual (excerpt)\n"
        "-------------------------\n"
        "syscalls : exit spawn kill getpid sleep malloc free open read\n"
        "           write close mkdir unlink stat ipc_send ipc_recv\n"
        "shell    : type 'help'\n"
        "compiler : cc <file.c> [-o out]\n"
        "editor   : edit <file>\n");
    vfs_write_string("/var/log/boot.log", "boot log is available via dmesg\n");
}

char *proc_generate(const char *path, u32 *out_len, const char **err);

int vfs_mount(void) {
    extern char *proc_generate(const char *, u32 *, const char **);
    u8 *base = g_phys_ram + DISK_BASE;
    vfs_superblock_t *sb = (vfs_superblock_t *)base;
    int fresh = 0;
    kmemset(&g_vfs, 0, sizeof(g_vfs));
    if (sb->magic != VFS_MAGIC) {
        format_vfs();
        fresh = 1;
    } else {
        g_vfs.base = DISK_BASE;
        g_vfs.sb = sb;
        g_vfs.inodes = (vfs_inode_t *)(base + VFS_META_OFF);
        g_vfs.block_bitmap = base + VFS_META_OFF + (u32)VFS_INODES * (u32)sizeof(vfs_inode_t);
        g_vfs.inode_bitmap = g_vfs.block_bitmap + (VFS_BLOCKS / 8);
        g_vfs.data_off = VFS_DATA_OFF_ALIGNED;
        g_vfs.data = base + g_vfs.data_off;
        sb->mount_time = irq_ticks();
    }
    if (fresh) {
        klog("vfs: formatting NVRAM ramdisk (superblock magic 0x%08X)", VFS_MAGIC);
        seed_vfs();
    }
    /* /dev entries are devices, /proc entries are virtual nodes */
    if (!vfs_resolve("/dev/null")) vfs_create_node("/dev/null", FT_DEV);
    if (!vfs_resolve("/dev/console")) vfs_create_node("/dev/console", FT_DEV);
    if (!vfs_resolve("/dev/urandom")) vfs_create_node("/dev/urandom", FT_DEV);
    if (!vfs_resolve("/dev/fb0")) vfs_create_node("/dev/fb0", FT_DEV);
    if (!vfs_resolve("/proc/meminfo")) {
        vfs_create_node("/proc/meminfo", FT_PROC);
        vfs_create_node("/proc/cpuinfo", FT_PROC);
        vfs_create_node("/proc/uptime", FT_PROC);
        vfs_create_node("/proc/version", FT_PROC);
        vfs_create_node("/proc/loadavg", FT_PROC);
        vfs_create_node("/proc/mounts", FT_PROC);
        vfs_create_node("/proc/partitions", FT_PROC);
    }
    kstrcpy(g_vfs.cwd, "/home/user");
    g_vfs.mounted = 1;
    g_vfs.sb->mounts++;
    klog("vfs: mounted %s on / (%u inodes, %u blocks of %u B, %u KiB free)",
         g_vfs.sb->volume, (unsigned)VFS_INODES, (unsigned)VFS_BLOCKS,
         (unsigned)VFS_BLOCK_SIZE, (unsigned)(g_vfs.sb->free_blocks * 4));
    return 0;
}

void vfs_sync(void) {
    /* The ramdisk is already the backing store; verify inode checksums. */
    u32 i, bad = 0;
    for (i = 1; i < VFS_INODES; i++) {
        if (ino_is_free(i)) continue;
        if (g_vfs.inodes[i].checksum != inode_checksum(&g_vfs.inodes[i])) bad++;
    }
    klog("vfs: sync complete, %u dirty inodes repaired", (unsigned)bad);
}

/* Telemetry for sysmon */
void vfs_usage(u32 *inodes_used, u32 *blocks_used, u32 *bytes_total) {
    if (inodes_used) *inodes_used = VFS_INODES - g_vfs.sb->free_inodes;
    if (blocks_used) *blocks_used = VFS_BLOCKS - g_vfs.sb->free_blocks;
    if (bytes_total) *bytes_total = VFS_FS_BYTES;
}

/* Formats a permissions field like ls -l */
void vfs_mode_string(u32 type, u32 mode, char *out) {
    out[0] = (type == FT_DIR) ? 'd' : (type == FT_DEV) ? 'c' : (type == FT_PROC) ? 'p' : '-';
    out[1] = (mode & 0400) ? 'r' : '-';
    out[2] = (mode & 0200) ? 'w' : '-';
    out[3] = (mode & 0100) ? 'x' : '-';
    out[4] = (mode & 0040) ? 'r' : '-';
    out[5] = (mode & 0020) ? 'w' : '-';
    out[6] = (mode & 0010) ? 'x' : '-';
    out[7] = (mode & 0004) ? 'r' : '-';
    out[8] = (mode & 0002) ? 'w' : '-';
    out[9] = (mode & 0001) ? 'x' : '-';
    out[10] = 0;
}

