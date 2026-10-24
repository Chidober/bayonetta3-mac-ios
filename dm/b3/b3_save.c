// B3 的存档：fsp-srv 51 OpenSaveDataFileSystem 回一个映射到宿主目录的 IFileSystem，命令号按 switchbrew「Filesystem services」IFileSystem / IFile / IDirectory，
// 参数布局按 libnx fs.c 的公开接口（ISC，只取布局）。数据目录 $B3_SAVE_DIR，缺省 ~/b3-work/saves/b3，按用户分子目录 <uid 高位>-<uid 低位>（用户号为 0 时是 common）；永远不在仓库、不在 B3_INPUT 里。
// iOS 上之后改成 app 容器路径（之后）。写入直接落盘，Commit 不做事（DECISION：不模拟存档的原子提交）。路径里的 ".." 段一律拒绝。
// B3 save data : fsp-srv 51 OpenSaveDataFileSystem returns an IFileSystem mapped to a host directory; command ids follow the switchbrew page "Filesystem services" IFileSystem / IFile / IDirectory,
// argument layouts follow libnx fs.c's public interface (ISC, layout only). Data lives in $B3_SAVE_DIR, default ~/b3-work/saves/b3, one subdirectory per user <uid high>-<uid low> ("common" for user 0); never in the repository or B3_INPUT.
// On iOS this becomes an app container path later (after ). Writes go straight to disk and Commit does nothing (DECISION: atomic save commits are not emulated). Any ".." path segment is refused.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dm.h"
#include "b3_ipc.h"

#define FS_PATH_NOT_FOUND 0x202       // FS 模块 2 描述 1 | FS module 2 description 1
#define FS_PATH_EXISTS 0x402          // FS 模块 2 描述 2 | FS module 2 description 2
#define FS_INVALID 0x2c99             // 参数无效一类 | invalid-argument-like
#define FS_MAX_FS 16
#define FS_MAX_FILES 128
#define FS_MAX_DIRS 32

typedef struct { int used; char root[1024]; } save_fs_t;
typedef struct { int used, fd, mode; } save_file_t;
typedef struct { char name[0x301]; uint8_t is_dir; uint64_t size; } save_ent_t;
typedef struct { int used, n, pos; save_ent_t *e; } save_dir_t;
static save_fs_t g_fs[FS_MAX_FS];
static save_file_t g_file[FS_MAX_FILES];
static save_dir_t g_dir[FS_MAX_DIRS];

static int inst(const b3_call_t *c) { const char *h = c->self ? strchr(c->self, '#') : NULL; return h ? atoi(h + 1) : -1; }
static save_fs_t *fs_of(const b3_call_t *c) { int k = inst(c); if (k < 0 || k >= FS_MAX_FS || !g_fs[k].used) die("B3 存档：未知文件系统实例 %s | unknown filesystem instance", c->self ? c->self : "(null)"); return &g_fs[k]; }
static save_file_t *file_of(const b3_call_t *c) { int k = inst(c); if (k < 0 || k >= FS_MAX_FILES || !g_file[k].used) die("B3 存档：未知文件实例 %s | unknown file instance", c->self ? c->self : "(null)"); return &g_file[k]; }
static save_dir_t *dir_of(const b3_call_t *c) { int k = inst(c); if (k < 0 || k >= FS_MAX_DIRS || !g_dir[k].used) die("B3 存档：未知目录实例 %s | unknown directory instance", c->self ? c->self : "(null)"); return &g_dir[k]; }

static int mkdirs(const char *path) {   // mkdir -p
    char tmp[1024]; strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}
// 用户的存档目录 | The user's save directory
static void user_dir(char out[1024], uint64_t lo, uint64_t hi) {
    const char *base = getenv("B3_SAVE_DIR"); char def[1024];
    if (!base || !*base) { snprintf(def, sizeof def, "%s/b3-work/saves/b3", getenv("HOME") ? getenv("HOME") : "."); base = def; }
    if (!lo && !hi) snprintf(out, 1024, "%s/common", base); else snprintf(out, 1024, "%s/%016llx-%016llx", base, (unsigned long long)hi, (unsigned long long)lo);
}
// 第 i 个路径缓冲 → 宿主路径；路径必须以 / 开头、不含 ".." 段 | The i-th path buffer → host path; the path must start with / and contain no ".." segment
static int host_path(const b3_call_t *c, unsigned i, char out[2048]) {
    b3_buf_t b = b3_in_buffer(c, i); if (!b.size) return -1;
    char p[0x302] = {0}; memcpy(p, (const void *)(uintptr_t)b.addr, b.size < 0x301 ? b.size : 0x301);
    if (p[0] != '/') return -1;
    for (const char *s = p; (s = strstr(s, "..")); s += 2) if ((s == p || s[-1] == '/') && (s[2] == '/' || !s[2])) return -1;
    snprintf(out, 2048, "%s%s", fs_of(c)->root, p);
    size_t n = strlen(out); while (n > 1 && out[n - 1] == '/' && n > strlen(fs_of(c)->root)) out[--n] = 0;
    return 0;
}
static uint32_t errno_result(void) { return errno == ENOENT || errno == ENOTDIR ? FS_PATH_NOT_FOUND : errno == EEXIST || errno == ENOTEMPTY ? FS_PATH_EXISTS : FS_INVALID; }

// ---------- fsp-srv ----------
// 51 OpenSaveDataFileSystem(u8 空间号, 填充, SaveDataAttribute 0x40 字节：+0 引用号、+8 Uid、+0x18 静态号、+0x20 类型) → IFileSystem
// 51 OpenSaveDataFileSystem(u8 space id, padding, SaveDataAttribute 0x40 bytes: +0 reference id, +8 Uid, +0x18 static id, +0x20 type) → IFileSystem
static uint32_t fsp_open_save(b3_call_t *c) {
    if (c->n_in < 2 + 16) return FS_INVALID;
    const uint32_t *a = c->in + 2; uint64_t lo = a[2] | (uint64_t)a[3] << 32, hi = a[4] | (uint64_t)a[5] << 32;
    int k = 0; while (k < FS_MAX_FS && g_fs[k].used) k++;
    if (k == FS_MAX_FS) die("B3 存档：文件系统实例用尽 | out of filesystem instances");
    user_dir(g_fs[k].root, lo, hi);
    if (mkdirs(g_fs[k].root)) die("B3 存档：建目录失败 %s | cannot create the save directory", g_fs[k].root);
    g_fs[k].used = 1;
    LOG("B3-SAVE open space=%u type=%u user=%s fs=#%d", c->in[0] & 0xFF, (unsigned)(a[8] & 0xFF), lo || hi ? "user" : "common", k);
    snprintf(c->obj, sizeof c->obj, "fs.IFileSystem#%d", k); return 0;
}
// IApplicationFunctions 20 EnsureSaveData(Uid) → u64 还缺的大小：建好目录，回 0 | → u64 size still needed: create the directory and return 0
static uint32_t af_ensure_save(b3_call_t *c) {
    if (c->n_in < 4) return FS_INVALID;
    char d[1024]; user_dir(d, c->in[0] | (uint64_t)c->in[1] << 32, c->in[2] | (uint64_t)c->in[3] << 32);
    if (mkdirs(d)) die("B3 存档：建目录失败 %s | cannot create the save directory", d);
    c->out[0] = c->out[1] = 0; c->n_out = 2; return 0;
}

// ---------- IFileSystem ----------
static uint32_t sfs_create_file(b3_call_t *c) {   // 0 CreateFile(u32 选项, 填充, s64 大小, 路径) | (u32 option, pad, s64 size, path)
    char p[2048]; if (c->n_in < 4 || host_path(c, 0, p)) return FS_INVALID;
    int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644); if (fd < 0) return errno_result();
    int64_t size = (int64_t)(c->in[2] | (uint64_t)c->in[3] << 32); int r = ftruncate(fd, size); close(fd);
    return r ? FS_INVALID : 0;
}
static uint32_t sfs_delete_file(b3_call_t *c) { char p[2048]; if (host_path(c, 0, p)) return FS_INVALID; return unlink(p) ? errno_result() : 0; }
static uint32_t sfs_create_dir(b3_call_t *c) { char p[2048]; if (host_path(c, 0, p)) return FS_INVALID; return mkdir(p, 0755) ? errno_result() : 0; }
static uint32_t sfs_delete_dir(b3_call_t *c) { char p[2048]; if (host_path(c, 0, p)) return FS_INVALID; return rmdir(p) ? errno_result() : 0; }
static int remove_tree(const char *p, int keep_self) {
    DIR *d = opendir(p); if (!d) return -1;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[2048]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); struct stat st;
        if (lstat(q, &st)) continue;
        if (S_ISDIR(st.st_mode)) remove_tree(q, 0); else unlink(q);
    }
    closedir(d); return keep_self ? 0 : rmdir(p);
}
static uint32_t sfs_delete_dir_rec(b3_call_t *c) { char p[2048]; if (host_path(c, 0, p)) return FS_INVALID; return remove_tree(p, 0) ? errno_result() : 0; }
static uint32_t sfs_clean_dir(b3_call_t *c) { char p[2048]; if (host_path(c, 0, p)) return FS_INVALID; return remove_tree(p, 1) ? errno_result() : 0; }
static uint32_t sfs_rename(b3_call_t *c) {   // 5 / 6 Rename(旧路径, 新路径) | (old path, new path)
    char a[2048], b[2048]; if (host_path(c, 0, a) || host_path(c, 1, b)) return FS_INVALID;
    struct stat st; if (!stat(b, &st)) return FS_PATH_EXISTS;
    return rename(a, b) ? errno_result() : 0;
}
static uint32_t sfs_entry_type(b3_call_t *c) {   // 7 GetEntryType(路径) → u32：0 目录、1 文件 | (path) → u32: 0 directory, 1 file
    char p[2048]; struct stat st; if (host_path(c, 0, p)) return FS_INVALID;
    if (stat(p, &st)) return FS_PATH_NOT_FOUND;
    c->out[0] = S_ISDIR(st.st_mode) ? 0 : 1; c->n_out = 1; return 0;
}
static uint32_t sfs_open_file(b3_call_t *c) {   // 8 OpenFile(u32 模式：1 读、2 写、4 追加, 路径) → IFile | (u32 mode: 1 read, 2 write, 4 append, path)
    char p[2048]; if (!c->n_in || host_path(c, 0, p)) return FS_INVALID;
    struct stat st; if (stat(p, &st)) return FS_PATH_NOT_FOUND;
    if (S_ISDIR(st.st_mode)) return FS_PATH_NOT_FOUND;
    uint32_t mode = c->in[0]; int fd = open(p, mode & 2 ? O_RDWR : O_RDONLY); if (fd < 0) return errno_result();
    int k = 0; while (k < FS_MAX_FILES && g_file[k].used) k++;
    if (k == FS_MAX_FILES) die("B3 存档：文件实例用尽 | out of file instances");
    g_file[k] = (save_file_t){1, fd, (int)mode};
    snprintf(c->obj, sizeof c->obj, "fs.IFile#%d", k); return 0;
}
static uint32_t sfs_open_dir(b3_call_t *c) {   // 9 OpenDirectory(u32 过滤：1 目录、2 文件, 路径) → IDirectory；打开时取快照 | (u32 filter: 1 dirs, 2 files, path); snapshot taken at open
    char p[2048]; if (!c->n_in || host_path(c, 0, p)) return FS_INVALID;
    DIR *d = opendir(p); if (!d) return errno_result();
    int k = 0; while (k < FS_MAX_DIRS && g_dir[k].used) k++;
    if (k == FS_MAX_DIRS) die("B3 存档：目录实例用尽 | out of directory instances");
    save_dir_t *o = &g_dir[k]; memset(o, 0, sizeof *o); o->used = 1;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char q[4096]; snprintf(q, sizeof q, "%s/%s", p, e->d_name); struct stat st; if (stat(q, &st)) continue;
        int is_dir = S_ISDIR(st.st_mode); if (!(c->in[0] & (is_dir ? 1 : 2))) continue;
        o->e = realloc(o->e, (o->n + 1) * sizeof *o->e); save_ent_t *t = &o->e[o->n++];
        memset(t, 0, sizeof *t); strlcpy(t->name, e->d_name, sizeof t->name); t->is_dir = (uint8_t)is_dir; t->size = is_dir ? 0 : (uint64_t)st.st_size;
    }
    closedir(d);
    snprintf(c->obj, sizeof c->obj, "fs.IDirectory#%d", k); return 0;
}
static uint32_t sfs_commit(b3_call_t *c) { (void)fs_of(c); return 0; }
static uint32_t sfs_space(b3_call_t *c) { (void)fs_of(c); uint64_t n = 512ull << 20; memcpy(c->out, &n, 8); c->n_out = 2; return 0; }   // 11 / 12：报 512 MiB（DECISION）| report 512 MiB (DECISION)

// ---------- IFile ----------
static uint32_t file_read(b3_call_t *c) {   // 0 Read(u32 选项, 填充, s64 偏移, u64 大小, 输出缓冲) → u64 读到的字节 | (u32 option, pad, s64 offset, u64 size, out buffer) → u64 bytes read
    save_file_t *f = file_of(c); if (c->n_in < 6) return FS_INVALID;
    int64_t off = (int64_t)(c->in[2] | (uint64_t)c->in[3] << 32); uint64_t n = c->in[4] | (uint64_t)c->in[5] << 32;
    b3_buf_t b = b3_out_buffer(c, 0); if (n > b.size) n = b.size;
    ssize_t r = pread(f->fd, (void *)(uintptr_t)b.addr, n, off); if (r < 0) return FS_INVALID;
    uint64_t got = (uint64_t)r; memcpy(c->out, &got, 8); c->n_out = 2; return 0;
}
static uint32_t file_write(b3_call_t *c) {   // 1 Write(u32 选项, 填充, s64 偏移, u64 大小, 输入缓冲)；不带追加位时不能写过文件尾 | (u32 option, pad, s64 offset, u64 size, in buffer); without the append bit, writing past the end fails
    save_file_t *f = file_of(c); if (c->n_in < 6) return FS_INVALID;
    if (!(f->mode & 2)) return FS_INVALID;
    int64_t off = (int64_t)(c->in[2] | (uint64_t)c->in[3] << 32); uint64_t n = c->in[4] | (uint64_t)c->in[5] << 32;
    b3_buf_t b = b3_in_buffer(c, 0); if (n > b.size) return FS_INVALID;
    struct stat st; fstat(f->fd, &st);
    if (!(f->mode & 4) && (uint64_t)off + n > (uint64_t)st.st_size) return 0x307202;   // 文档：没有追加位时写过文件尾报 0x307202 | docs: 0x307202 when writing past the end without the append bit
    return pwrite(f->fd, (const void *)(uintptr_t)b.addr, n, off) == (ssize_t)n ? 0 : FS_INVALID;
}
static uint32_t file_flush(b3_call_t *c) { return fsync(file_of(c)->fd) ? FS_INVALID : 0; }
static uint32_t file_set_size(b3_call_t *c) { save_file_t *f = file_of(c); if (c->n_in < 2) return FS_INVALID; return ftruncate(f->fd, (off_t)(c->in[0] | (uint64_t)c->in[1] << 32)) ? FS_INVALID : 0; }
static uint32_t file_get_size(b3_call_t *c) { struct stat st; if (fstat(file_of(c)->fd, &st)) return FS_INVALID; uint64_t n = (uint64_t)st.st_size; memcpy(c->out, &n, 8); c->n_out = 2; return 0; }

// ---------- IDirectory ----------
static uint32_t dir_read(b3_call_t *c) {   // 0 Read(输出缓冲：DirectoryEntry 每项 0x310) → u64 个数 | (out buffer: DirectoryEntry, 0x310 each) → u64 count
    save_dir_t *d = dir_of(c); b3_buf_t b = b3_out_buffer(c, 0); uint64_t n = 0;
    while (d->pos < d->n && (n + 1) * 0x310 <= b.size) {
        uint8_t *o = (uint8_t *)(uintptr_t)b.addr + n * 0x310; const save_ent_t *e = &d->e[d->pos++];
        memset(o, 0, 0x310); memcpy(o, e->name, 0x301); o[0x301] = e->is_dir; o[0x304] = e->is_dir ? 0 : 1; memcpy(o + 0x308, &e->size, 8); n++;
    }
    memcpy(c->out, &n, 8); c->n_out = 2; return 0;
}
static uint32_t dir_count(b3_call_t *c) { save_dir_t *d = dir_of(c); uint64_t n = (uint64_t)(d->n - d->pos); memcpy(c->out, &n, 8); c->n_out = 2; return 0; }

// 对象关闭：释放文件描述符与实例槽 | Object closed: release the file descriptor and the instance slot
void b3_object_closed(const char *name) {
    const char *h = strchr(name, '#'); if (!h) return; int k = atoi(h + 1);
    if (!strncmp(name, "fs.IFile#", 9) && k >= 0 && k < FS_MAX_FILES && g_file[k].used) { close(g_file[k].fd); g_file[k].used = 0; }
    else if (!strncmp(name, "fs.IDirectory#", 14) && k >= 0 && k < FS_MAX_DIRS && g_dir[k].used) { free(g_dir[k].e); memset(&g_dir[k], 0, sizeof g_dir[k]); }
    else if (!strncmp(name, "fs.IFileSystem#", 15) && k >= 0 && k < FS_MAX_FS) g_fs[k].used = 0;
}

const b3_cmd_t b3_save_cmds[] = {
    {"fsp-srv", B3_CMIF, 51, fsp_open_save, "switchbrew FS: IFileSystemProxy OpenSaveDataFileSystem"},
    {"am.IApplicationFunctions", B3_CMIF, 20, af_ensure_save, "switchbrew AM: IApplicationFunctions EnsureSaveData"},
    {"fs.IFileSystem", B3_CMIF, 0, sfs_create_file, "switchbrew FS: IFileSystem CreateFile"},
    {"fs.IFileSystem", B3_CMIF, 1, sfs_delete_file, "switchbrew FS: IFileSystem DeleteFile"},
    {"fs.IFileSystem", B3_CMIF, 2, sfs_create_dir, "switchbrew FS: IFileSystem CreateDirectory"},
    {"fs.IFileSystem", B3_CMIF, 3, sfs_delete_dir, "switchbrew FS: IFileSystem DeleteDirectory"},
    {"fs.IFileSystem", B3_CMIF, 4, sfs_delete_dir_rec, "switchbrew FS: IFileSystem DeleteDirectoryRecursively"},
    {"fs.IFileSystem", B3_CMIF, 5, sfs_rename, "switchbrew FS: IFileSystem RenameFile"},
    {"fs.IFileSystem", B3_CMIF, 6, sfs_rename, "switchbrew FS: IFileSystem RenameDirectory"},
    {"fs.IFileSystem", B3_CMIF, 7, sfs_entry_type, "switchbrew FS: IFileSystem GetEntryType"},
    {"fs.IFileSystem", B3_CMIF, 8, sfs_open_file, "switchbrew FS: IFileSystem OpenFile"},
    {"fs.IFileSystem", B3_CMIF, 9, sfs_open_dir, "switchbrew FS: IFileSystem OpenDirectory"},
    {"fs.IFileSystem", B3_CMIF, 10, sfs_commit, "switchbrew FS: IFileSystem Commit"},
    {"fs.IFileSystem", B3_CMIF, 11, sfs_space, "switchbrew FS: IFileSystem GetFreeSpaceSize"},
    {"fs.IFileSystem", B3_CMIF, 12, sfs_space, "switchbrew FS: IFileSystem GetTotalSpaceSize"},
    {"fs.IFileSystem", B3_CMIF, 13, sfs_clean_dir, "switchbrew FS: IFileSystem CleanDirectoryRecursively"},
    {"fs.IFile", B3_CMIF, 0, file_read, "switchbrew FS: IFile Read"},
    {"fs.IFile", B3_CMIF, 1, file_write, "switchbrew FS: IFile Write"},
    {"fs.IFile", B3_CMIF, 2, file_flush, "switchbrew FS: IFile Flush"},
    {"fs.IFile", B3_CMIF, 3, file_set_size, "switchbrew FS: IFile SetSize"},
    {"fs.IFile", B3_CMIF, 4, file_get_size, "switchbrew FS: IFile GetSize"},
    {"fs.IDirectory", B3_CMIF, 0, dir_read, "switchbrew FS: IDirectory Read"},
    {"fs.IDirectory", B3_CMIF, 1, dir_count, "switchbrew FS: IDirectory GetEntryCount"},
};
const unsigned b3_save_ncmds = sizeof b3_save_cmds / sizeof *b3_save_cmds;
