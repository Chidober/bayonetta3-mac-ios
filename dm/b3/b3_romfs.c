// B3 的虚拟 RomFS 映像：游戏经 fsp-srv OpenDataStorageByCurrentProcess 按偏移读原始 RomFS，而本机输入是解开的目录树（$B3_INPUT/RomFS）。
// 这里按公开的 RomFS 格式（libnx romfs_dev.h 的 romfs_header / romfs_dir / romfs_file，romfs_dev.c 的 calcHash）在内存里建元数据，文件数据按需从宿主文件读，不把 14 GB 拷成映像。
// 布局：头 0x50 → 目录散列表 → 目录表 → 文件散列表 → 文件表 → 文件数据（0x200 对齐起，每个文件 0x10 对齐）。目录、文件按名字字节序排列，映像确定。
// B3 virtual RomFS image: the game reads raw RomFS by offset through fsp-srv OpenDataStorageByCurrentProcess, while the local input is an extracted directory tree ($B3_INPUT/RomFS).
// This builds the metadata in memory per the public RomFS format (romfs_header / romfs_dir / romfs_file in libnx romfs_dev.h, calcHash in romfs_dev.c) and reads file data from host files on demand, without copying 14 GB into an image.
// Layout: header 0x50 → directory hash table → directory table → file hash table → file table → file data (starting 0x200-aligned, each file 0x10-aligned). Directories and files are ordered by byte-wise name, so the image is deterministic.
#include "dm.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE 0xffffffffu
typedef struct { char *host; uint64_t off, size; int fd; } b3_rfile_t;
typedef struct b3_romfs {
    uint8_t *meta; uint64_t meta_size, data_off, total;   // [0, meta_size) 在内存；[data_off, total) 是文件数据 | [0, meta_size) in memory; [data_off, total) is file data
    b3_rfile_t *files; unsigned nfiles;                    // 按数据偏移升序 | ascending data offset
} b3_romfs_t;

// ---- 构建时的树 | Tree used while building ----
typedef struct node { char *name, *host; int is_dir; uint64_t size; struct node **kids; unsigned nkids; uint32_t off; uint64_t data_off; } node_t;
static int cmp_node(const void *a, const void *b) { return strcmp((*(node_t *const *)a)->name, (*(node_t *const *)b)->name); }
static node_t *scan(const char *host, const char *name) {
    node_t *n = calloc(1, sizeof *n); n->name = strdup(name); n->host = strdup(host); n->is_dir = 1;
    DIR *d = opendir(host); if (!d) die("B3 RomFS：打不开目录 | cannot open directory %s", host);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;   // 跳过 . .. 和 Finder 的 .DS_Store | skip . .. and Finder's .DS_Store
        char p[4096]; snprintf(p, sizeof p, "%s/%s", host, e->d_name);
        struct stat st; if (stat(p, &st)) continue;
        node_t *k;
        if (S_ISDIR(st.st_mode)) k = scan(p, e->d_name);
        else { k = calloc(1, sizeof *k); k->name = strdup(e->d_name); k->host = strdup(p); k->size = (uint64_t)st.st_size; }
        n->kids = realloc(n->kids, (n->nkids + 1) * sizeof *n->kids); n->kids[n->nkids++] = k;
    }
    closedir(d);
    if (n->nkids) qsort(n->kids, n->nkids, sizeof *n->kids, cmp_node);
    return n;
}
static uint32_t entry_size(const node_t *n, int is_dir) { return (is_dir ? 0x18 : 0x20) + ((uint32_t)strlen(n->name) + 3) / 4 * 4; }
static uint32_t calc_hash(uint32_t parent, const char *name, uint32_t buckets) {   // libnx romfs_dev.c calcHash
    uint32_t h = parent ^ 123456789; for (const unsigned char *p = (const unsigned char *)name; *p; p++) { h = (h >> 5) | (h << 27); h ^= *p; }
    return h % buckets;
}
static uint32_t buckets_for(uint32_t n) { uint32_t b = n < 3 ? 3 : n | 1; for (;; b += 2) { uint32_t d = 3; while (d * d <= b && b % d) d += 2; if (d * d > b) return b; } }

// 第一遍：给每个目录 / 文件分表内偏移、给文件分数据偏移 | Pass 1: assign table offsets to directories / files and data offsets to files
static void layout(node_t *n, uint32_t *dir_off, uint32_t *file_off, uint64_t *data, unsigned *nd, unsigned *nf) {
    for (unsigned i = 0; i < n->nkids; i++) if (n->kids[i]->is_dir) { n->kids[i]->off = *dir_off; *dir_off += entry_size(n->kids[i], 1); (*nd)++; }
    for (unsigned i = 0; i < n->nkids; i++) if (!n->kids[i]->is_dir) {
        node_t *f = n->kids[i]; f->off = *file_off; *file_off += entry_size(f, 0); (*nf)++;
        *data = (*data + 15) & ~15ull; f->data_off = *data; *data += f->size;
    }
    for (unsigned i = 0; i < n->nkids; i++) if (n->kids[i]->is_dir) layout(n->kids[i], dir_off, file_off, data, nd, nf);
}
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
// 第二遍：写目录 / 文件表项并挂进散列表 | Pass 2: write directory / file entries and link them into the hash tables
static void emit(b3_romfs_t *r, node_t *n, uint32_t parent, uint8_t *dhash, uint32_t dbk, uint8_t *dtab, uint8_t *fhash, uint32_t fbk, uint8_t *ftab, unsigned *fi) {
    uint32_t first_dir = NONE, first_file = NONE;
    for (unsigned i = n->nkids; i-- > 0;) { if (n->kids[i]->is_dir) first_dir = n->kids[i]->off; }
    for (unsigned i = n->nkids; i-- > 0;) { if (!n->kids[i]->is_dir) first_file = n->kids[i]->off; }
    uint8_t *e = dtab + n->off; put32(e + 8, first_dir); put32(e + 12, first_file);
    for (unsigned i = 0; i < n->nkids; i++) {
        node_t *k = n->kids[i]; uint32_t sib = NONE;
        for (unsigned j = i + 1; j < n->nkids; j++) if (n->kids[j]->is_dir == k->is_dir) { sib = n->kids[j]->off; break; }
        uint32_t len = (uint32_t)strlen(k->name);
        if (k->is_dir) {
            uint8_t *d = dtab + k->off; uint32_t b = calc_hash(n->off, k->name, dbk);
            put32(d, n->off); put32(d + 4, sib); put32(d + 16, *(uint32_t *)(dhash + 4 * b)); put32(d + 20, len); memcpy(d + 24, k->name, len);
            put32(dhash + 4 * b, k->off);
        } else {
            uint8_t *f = ftab + k->off; uint32_t b = calc_hash(n->off, k->name, fbk);
            put32(f, n->off); put32(f + 4, sib); put64(f + 8, k->data_off); put64(f + 16, k->size); put32(f + 24, *(uint32_t *)(fhash + 4 * b)); put32(f + 28, len); memcpy(f + 32, k->name, len);
            put32(fhash + 4 * b, k->off);
            r->files[(*fi)++] = (b3_rfile_t){strdup(k->host), k->data_off, k->size, -1};
        }
    }
    (void)parent;
    for (unsigned i = 0; i < n->nkids; i++) if (n->kids[i]->is_dir) emit(r, n->kids[i], n->off, dhash, dbk, dtab, fhash, fbk, ftab, fi);
}
static int cmp_file(const void *a, const void *b) { const b3_rfile_t *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off; }

b3_romfs_t *b3_romfs_open(const char *root) {
    struct stat st; if (stat(root, &st) || !S_ISDIR(st.st_mode)) return NULL;
    node_t *top = scan(root, "");
    uint32_t dir_off = entry_size(top, 1), file_off = 0; uint64_t data = 0; unsigned nd = 1, nf = 0;
    top->off = 0; layout(top, &dir_off, &file_off, &data, &nd, &nf);
    uint32_t dbk = buckets_for(nd), fbk = buckets_for(nf);
    uint64_t dh = 0x50, dt = dh + 4ull * dbk, fh = dt + dir_off, ft = fh + 4ull * fbk, end = ft + file_off;
    b3_romfs_t *r = calloc(1, sizeof *r);
    r->meta_size = end; r->data_off = (end + 0x1ff) & ~0x1ffull; r->total = r->data_off + data; r->meta = calloc(1, end);
    r->files = calloc(nf ? nf : 1, sizeof *r->files);
    uint8_t *h = r->meta;
    put64(h, 0x50); put64(h + 8, dh); put64(h + 16, 4ull * dbk); put64(h + 24, dt); put64(h + 32, dir_off);
    put64(h + 40, fh); put64(h + 48, 4ull * fbk); put64(h + 56, ft); put64(h + 64, file_off); put64(h + 72, r->data_off);
    memset(h + dh, 0xff, 4ull * dbk); memset(h + fh, 0xff, 4ull * fbk);
    uint8_t *root_e = h + dt; put32(root_e, 0); put32(root_e + 4, NONE); put32(root_e + 16, NONE); put32(root_e + 20, 0);   // 根目录：父为自己、名字长 0 | root: parent is itself, name length 0
    put32(h + dh + 4 * calc_hash(0, "", dbk), 0);
    unsigned fi = 0; emit(r, top, 0, h + dh, dbk, h + dt, h + fh, fbk, h + ft, &fi); r->nfiles = fi;
    qsort(r->files, r->nfiles, sizeof *r->files, cmp_file);
    LOG("B3-ROMFS dirs=%u files=%u meta=%#llx data_off=%#llx total=%#llx", nd, nf, (unsigned long long)r->meta_size, (unsigned long long)r->data_off, (unsigned long long)r->total);
    return r;
}
uint64_t b3_romfs_size(const b3_romfs_t *r) { return r->total; }
// b3_movie.c 提供（单测程序不链它，用这个空的弱定义）| provided by b3_movie.c (test programs do not link it and use this empty weak definition)
__attribute__((weak)) void b3_movie_file(const char *host) { (void)host; }

// 读 [off, off+n)：元数据区从内存拷，数据区按文件拼，文件间的对齐空隙填 0；返回实际读到的字节数 | Read [off, off+n): metadata from memory, data stitched from files, alignment gaps between files are zero; returns the bytes read
uint64_t b3_romfs_read(b3_romfs_t *r, uint64_t off, void *buf, uint64_t n) {
    if (off >= r->total) return 0;
    if (n > r->total - off) n = r->total - off;
    uint8_t *out = buf; memset(out, 0, n);
    if (off < r->meta_size) memcpy(out, r->meta + off, (off + n < r->meta_size ? n : r->meta_size - off));
    if (off + n <= r->data_off) return n;
    uint64_t lo = off > r->data_off ? off - r->data_off : 0, hi = off + n - r->data_off;   // 数据区内的相对区间 | relative range within the data area
    unsigned a = 0, b = r->nfiles;
    while (a < b) { unsigned m = (a + b) / 2; if (r->files[m].off + r->files[m].size <= lo) a = m + 1; else b = m; }
    for (unsigned i = a; i < r->nfiles && r->files[i].off < hi; i++) {
        b3_rfile_t *f = &r->files[i];
        uint64_t s = lo > f->off ? lo : f->off, e = hi < f->off + f->size ? hi : f->off + f->size;
        if (s >= e) continue;
        if (f->fd < 0) { if ((f->fd = open(f->host, O_RDONLY)) < 0) die("B3 RomFS：打不开文件 | cannot open file %s", f->host); b3_movie_file(f->host); }   // 第一次读到某文件：影片模块按扩展名决定要不要索引| first read of a file: the movie module decides by extension whether to index it 
        ssize_t got = pread(f->fd, out + (s + r->data_off - off), e - s, (off_t)(s - f->off));
        if (got != (ssize_t)(e - s)) die("B3 RomFS：读文件失败 | file read failed %s", f->host);
    }
    return n;
}
