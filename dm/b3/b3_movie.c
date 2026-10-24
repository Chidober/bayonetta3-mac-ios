// B3 影片解码：游戏把 RomFS 里的 webm（全是 VP9，统计）交给 NVDEC 解，NVDEC 只收到「去掉未压缩头」的码流（驱动在 CPU 上解析了头部），
// 宿主解码器（VideoToolbox）要整帧，所以按内容对齐：
//   1. RomFS 第一次打开 .webm 时（b3_romfs.c 回调）解析 Matroska，切出每个块里的子帧（VP9 超帧按尾部索引拆），按子帧末尾 16 字节建索引；
//   2. NVDEC 每次执行（b3_host1x.c 回调）：图像信息 +0x30 是码流长度，按码流末尾在索引里找到（文件, 块），记下当前输出面（亮度槽 3）属于它；
//      该文件的 VideoToolbox 会话按顺序解到这一块（往回跳时从前一个关键帧重解），保留最近几块的画面；
//   3. VIC 执行（b3_host1x.c 回调）：按输入面查到（文件, 块）对应的画面，按 NV12 写进 VIC 输出面。
// 每个块的画面就是 VideoToolbox 对整块（含超帧）输出的那一帧；显示已有帧（show_existing_frame）不走 NVDEC，此时 VIC 读到的是之前那块的画面（可能差一帧，DECISION）。
// 游戏文件只读、只在宿主内存里，不写出。
// B3 movie decoding : the game hands the webm files in RomFS (all VP9, count) to NVDEC, and NVDEC only receives the bitstream "minus the uncompressed header" (the driver parsed the header on the CPU),
// while the host decoder (VideoToolbox) needs whole frames, so the two are aligned by content:
//   1. when RomFS first opens a .webm (callback from b3_romfs.c), parse the Matroska file, split each block into subframes (VP9 superframes by their trailing index) and index them by the last 16 bytes of each subframe;
//   2. on every NVDEC execute (callback from b3_host1x.c): picture info +0x30 is the bitstream length; look up (file, block) by the bitstream's tail and record that the current output surface (luma slot 3) belongs to it;
//      that file's VideoToolbox session decodes in order up to that block (re-decoding from the previous keyframe when jumping back) and keeps the pictures of the last few blocks;
//   3. on VIC execute (callback from b3_host1x.c): find the picture for the input surface's (file, block) and write it as NV12 into the VIC output surface.
// A block's picture is the one frame VideoToolbox outputs for the whole block (superframes included); show_existing_frame does not go through NVDEC, and VIC then reads the earlier block's picture (possibly one frame off, DECISION).
// Game files are read only and stay in host memory; nothing is written out.
#include "dm.h"
#include <VideoToolbox/VideoToolbox.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint32_t off, size; uint8_t key; } blk_t;                  // 块在文件里的位置；key = 关键帧 | block position in the file; key = keyframe
typedef struct { uint32_t file, blk, sub_off, sub_size; } sub_t;          // 子帧（超帧拆开后）| subframe (after splitting superframes)
#define RING 8
typedef struct {
    char *host; uint8_t *data; size_t size; unsigned w, h;
    blk_t *blks; unsigned nblk;
    VTDecompressionSessionRef vt; CMVideoFormatDescriptionRef fd; int next;     // next = 下一块要解的序号 | next block to decode
    CVPixelBufferRef pic[RING]; int pic_blk[RING];                              // 最近几块的画面 | pictures of the last few blocks
    int cursor;                                                                  // 最近一次对上的块 | last matched block
} movie_t;
static movie_t g_mv[16]; static unsigned g_nmv;
static sub_t *g_sub; static unsigned g_nsub, g_capsub;
static struct { uint64_t luma; int file, blk; CVPixelBufferRef pic; } g_surf[64]; static unsigned g_nsurf;   // 输出面 → 画面（NVDEC 跑在 VIC 前面好几帧，画面跟着面走）| output surface → picture (NVDEC runs several frames ahead of VIC, so the picture travels with the surface)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// ---- Matroska（EBML）最小解析：只取视频轨的块 | minimal Matroska (EBML) parsing: video track blocks only ----
static int vint(const uint8_t *p, size_t n, uint64_t *v, int keep_marker) {
    if (!n || !p[0]) return 0; int len = __builtin_clz((unsigned)p[0]) - 23; if (len > 8 || (size_t)len > n) return 0;
    uint64_t x = keep_marker ? p[0] : p[0] & (0xffu >> len); for (int i = 1; i < len; i++) x = x << 8 | p[i];
    *v = x; return len;
}
static void mkv_walk(movie_t *m, const uint8_t *p, size_t n, unsigned vtrack) {
    size_t i = 0;
    while (i < n) {
        uint64_t id, sz; int a = vint(p + i, n - i, &id, 1); if (!a) return;
        int b = vint(p + i + a, n - i - a, &sz, 0); if (!b) return;
        size_t body = i + a + b; int unknown = sz == (1ull << (7 * b)) - 1;
        size_t end = unknown || sz > n - body ? n : body + sz;
        switch (id) {
        case 0x18538067: case 0x1F43B675: case 0xA0: case 0x1654AE6B: case 0xAE: case 0xE0: mkv_walk(m, p + body, end - body, vtrack); break;   // Segment Cluster BlockGroup Tracks TrackEntry Video
        case 0xB0: { uint64_t v = 0; for (size_t k = body; k < end; k++) v = v << 8 | p[k]; if (!m->w) m->w = (unsigned)v; } break;   // PixelWidth
        case 0xBA: { uint64_t v = 0; for (size_t k = body; k < end; k++) v = v << 8 | p[k]; if (!m->h) m->h = (unsigned)v; } break;   // PixelHeight
        case 0xA3: case 0xA1: {   // SimpleBlock / Block：轨号 vint、时间 2 字节、标志 1 字节、数据（视频不用 lacing）| track vint, timecode 2 bytes, flags 1 byte, data (video uses no lacing)
            uint64_t tr; int t = vint(p + body, end - body, &tr, 0); if (!t || body + t + 3 > end) break;
            uint8_t fl = p[body + t + 2]; if (fl & 0x06) break;
            if (vtrack && tr != vtrack) break;
            uint32_t off = (uint32_t)(p + body + t + 3 - m->data), size = (uint32_t)(end - (body + t + 3));
            int key = id == 0xA3 ? (fl & 0x80) != 0 : 0;
            if (id == 0xA1 || !key) { const uint8_t *f = m->data + off; if (size && (f[0] & 0xc0) == 0x80 && !(f[0] & 0x04)) key = 1; }   // 帧头：frame_marker=2、profile 0/1 时位 2 = 非关键帧 | header: frame_marker=2; with profile 0/1 bit 2 = non-keyframe
            m->blks = realloc(m->blks, (m->nblk + 1) * sizeof *m->blks); m->blks[m->nblk++] = (blk_t){off, size, (uint8_t)key};
        } break;
        }
        if (unknown && (id == 0x18538067 || id == 0x1F43B675)) return;   // 未知长度：子元素已读到本层末尾 | unknown size: children were read to the end of this level
        i = end;
    }
}
static uint64_t tail_key(const uint8_t *p, uint32_t n) { uint64_t h = 1469598103934665603ull; uint32_t k = n < 16 ? n : 16; for (uint32_t i = n - k; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static void add_sub(unsigned file, unsigned blk, uint32_t off, uint32_t size) {
    if (g_nsub == g_capsub) { g_capsub = g_capsub ? g_capsub * 2 : 4096; g_sub = realloc(g_sub, g_capsub * sizeof *g_sub); }
    g_sub[g_nsub++] = (sub_t){file, blk, off, size};
}
static void index_subframes(unsigned fi) {
    movie_t *m = &g_mv[fi];
    for (unsigned b = 0; b < m->nblk; b++) {
        const uint8_t *f = m->data + m->blks[b].off; uint32_t n = m->blks[b].size; if (!n) continue;
        uint8_t mk = f[n - 1];
        if ((mk & 0xe0) == 0xc0) {   // VP9 超帧索引（规范附录 B）| VP9 superframe index (spec annex B)
            unsigned frames = (mk & 7) + 1, bytes = ((mk >> 3) & 3) + 1, isz = 2 + bytes * frames;
            if (n >= isz && f[n - isz] == mk) { uint32_t o = 0;
                for (unsigned k = 0; k < frames; k++) { uint32_t s = 0; for (unsigned j = 0; j < bytes; j++) s |= (uint32_t)f[n - isz + 1 + k * bytes + j] << (8 * j);
                    if (o + s > n - isz) break; add_sub(fi, b, m->blks[b].off + o, s); o += s; }
                continue; }
        }
        add_sub(fi, b, m->blks[b].off, n);
    }
}

// romfs 回调：第一次打开某个 .webm 时调用 | romfs callback: called the first time a .webm is opened
void b3_movie_file(const char *host) {
    size_t hl = strlen(host); if (hl < 5 || strcmp(host + hl - 5, ".webm")) return;
    pthread_mutex_lock(&g_mu);
    for (unsigned i = 0; i < g_nmv; i++) if (!strcmp(g_mv[i].host, host)) { pthread_mutex_unlock(&g_mu); return; }
    if (g_nmv == 16) { LOG("B3-MOVIE 文件表满 | file table full"); pthread_mutex_unlock(&g_mu); return; }
    movie_t *m = &g_mv[g_nmv]; memset(m, 0, sizeof *m);
    int fd = open(host, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); pthread_mutex_unlock(&g_mu); return; }
    m->host = strdup(host); m->size = (size_t)st.st_size; m->data = malloc(m->size ? m->size : 1);
    if (pread(fd, m->data, m->size, 0) != (ssize_t)m->size) { close(fd); free(m->data); free(m->host); pthread_mutex_unlock(&g_mu); return; }
    close(fd);
    mkv_walk(m, m->data, m->size, 0);
    for (int k = 0; k < RING; k++) m->pic_blk[k] = -1;
    m->cursor = -1;
    unsigned fi = g_nmv++; index_subframes(fi);
    LOG("B3-MOVIE 打开 #%u %ux%u 块 %u 子帧累计 %u | opened", fi, m->w, m->h, m->nblk, g_nsub);
    pthread_mutex_unlock(&g_mu);
}

// ---- VideoToolbox ----
static void vt_out(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)fl; (void)pts; (void)dur; movie_t *m = ref; int blk = (int)(intptr_t)src;
    if (st || !img) return;
    int slot = blk % RING; if (m->pic[slot]) CVPixelBufferRelease(m->pic[slot]);
    m->pic[slot] = CVPixelBufferRetain(img); m->pic_blk[slot] = blk;
}
static int vt_open(movie_t *m) {
    if (m->vt) return 1;
    VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
    // vpcC（VP 编解码配置记录，版本 1）：档次 0、级别 0、8 位、4:2:0、BT.709 | vpcC (VP codec configuration record, version 1): profile 0, level 0, 8-bit, 4:2:0, BT.709
    uint8_t vpcc[12] = {1, 0, 0, 0, 0, 0, (8 << 4) | (1 << 1) | 0, 1, 1, 1, 0, 0};
    CFDataRef d = CFDataCreate(NULL, vpcc, sizeof vpcc);
    const void *ak[] = {CFSTR("vpcC")}, *av[] = {d}; CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus s = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_VP9, (int32_t)m->w, (int32_t)m->h, ext, &m->fd);
    CFRelease(ext); CFRelease(atoms); CFRelease(d);
    if (s) { LOG("B3-MOVIE 建格式描述失败 %d | format description failed", (int)s); return 0; }
    int pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &pf);
    const void *pk[] = {kCVPixelBufferPixelFormatTypeKey}, *pv[] = {n}; CFDictionaryRef attrs = CFDictionaryCreate(NULL, pk, pv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb = {vt_out, m};
    s = VTDecompressionSessionCreate(NULL, m->fd, NULL, attrs, &cb, &m->vt);
    CFRelease(attrs); CFRelease(n);
    if (s) { LOG("B3-MOVIE 建解码会话失败 %d | decompression session failed", (int)s); m->vt = NULL; return 0; }
    m->next = 0; return 1;
}
static void vt_reset(movie_t *m) { if (m->vt) { VTDecompressionSessionInvalidate(m->vt); CFRelease(m->vt); m->vt = NULL; } }
static void vt_decode_block(movie_t *m, int b) {
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t n = m->blks[b].size;
    if (CMBlockBufferCreateWithMemoryBlock(NULL, m->data + m->blks[b].off, n, kCFAllocatorNull, NULL, 0, n, 0, &bb)) return;
    if (!CMSampleBufferCreateReady(NULL, bb, m->fd, 1, 0, NULL, 1, &n, &sb)) {
        VTDecodeInfoFlags out; OSStatus s = VTDecompressionSessionDecodeFrame(m->vt, sb, 0, (void *)(intptr_t)b, &out);
        if (s) { static int k; if (k++ < 5) LOG("B3-MOVIE 解码失败 块 %d 状态 %d | decode failed", b, (int)s); }
        CFRelease(sb);
    }
    CFRelease(bb);
}
static void decode_to(movie_t *m, int b) {   // 顺序解到块 b；往回跳或跳得太远时从 b 之前最近的关键帧重来 | decode in order up to block b; when jumping back or far ahead, restart from the nearest keyframe before b
    if (m->vt && b == m->next - 1) return;   // 已解过 | already decoded
    if (!m->vt || b < m->next - 1 || b > m->next + 240) {
        int k = b; while (k > 0 && !m->blks[k].key) k--;
        if (!m->vt || b < m->next - 1 || k > m->next) { vt_reset(m); if (!vt_open(m)) return; m->next = k; }
    }
    while (m->next <= b) vt_decode_block(m, m->next++);
    VTDecompressionSessionWaitForAsynchronousFrames(m->vt);
}

// NVDEC 执行回调（b3_host1x.c）：m 是该通道的方法寄存器 | NVDEC execute callback (b3_host1x.c): m is the channel's method registers
void b3_movie_nvdec(int fd, const uint32_t *mr, uint64_t (*addr)(uint32_t, uint64_t *)) {
    (void)fd;
    if (mr[0x80] != 9) { static int k; if (k++ < 3) LOG("B3-MOVIE 编解码号 %u 不是 VP9，跳过 | codec id %u is not VP9, skipped", mr[0x80], mr[0x80]); return; }
    uint64_t av = 0; const uint32_t *pi = (const uint32_t *)(uintptr_t)addr(mr[0x101], &av);
    const uint8_t *bs = (const uint8_t *)(uintptr_t)addr(mr[0x102], NULL);
    if (!pi || av < 0x68 || !bs) return;
    uint32_t n = pi[0x30 / 4]; if (!n) return;
    uint64_t cur = (uint64_t)mr[0x10c + 3] << 8;   // 当前帧输出面：亮度槽 3（Ryujinx Vp9Decoder 同口径）| current frame output surface: luma slot 3 (as in Ryujinx's Vp9Decoder)
    pthread_mutex_lock(&g_mu);
    uint64_t key = tail_key(bs, n); int best = -1, bestscore = 1 << 30;
    for (unsigned i = 0; i < g_nsub; i++) {
        sub_t *s = &g_sub[i]; if (s->sub_size < n) continue;
        const uint8_t *f = g_mv[s->file].data + s->sub_off;
        if (tail_key(f, s->sub_size) != key && n >= 16) continue;
        if (memcmp(f + s->sub_size - n, bs, n)) continue;
        int dist = (int)s->blk - g_mv[s->file].cursor; int score = dist > 0 ? dist : 100000 - dist;   // 优先同文件游标之后最近的块 | prefer the nearest block after the file's cursor
        if (score < bestscore) { bestscore = score; best = (int)i; }
    }
    if (best < 0) { static int k; if (k++ < 5) LOG("B3-MOVIE 码流 %u 字节对不上任何子帧 | bitstream of %u bytes matches no subframe", n, n); pthread_mutex_unlock(&g_mu); return; }
    sub_t *s = &g_sub[best]; movie_t *m = &g_mv[s->file]; m->cursor = (int)s->blk;
    unsigned j = 0; while (j < g_nsurf && g_surf[j].luma != cur) j++;
    if (j == g_nsurf && g_nsurf < 64) { g_nsurf++; g_surf[j].pic = NULL; }
    if (j < 64) g_surf[j].luma = cur, g_surf[j].file = (int)s->file, g_surf[j].blk = (int)s->blk;
    decode_to(m, (int)s->blk);
    if (j < 64) {   // 这一块的画面（隐藏帧没有输出时取之前最近的一块）挂到输出面上 | attach this block's picture (for hidden frames without output, the nearest earlier block) to the output surface
        CVPixelBufferRef p = NULL; int pb = -1;
        for (int k = 0; k < RING; k++) if (m->pic[k] && m->pic_blk[k] <= (int)s->blk && m->pic_blk[k] > pb) { p = m->pic[k]; pb = m->pic_blk[k]; }
        if (p) { if (g_surf[j].pic) CVPixelBufferRelease(g_surf[j].pic); g_surf[j].pic = CVPixelBufferRetain(p); }
    }
    pthread_mutex_unlock(&g_mu);
}

// VIC 回调（b3_host1x.c）：把输入面对应的画面按 NV12 写出；返回 1 = 写了 | VIC callback (b3_host1x.c): write the picture for the input surface as NV12; returns 1 when written
int b3_movie_fill_nv12(uint64_t src_luma, uint8_t *luma, uint32_t lpitch, uint32_t lh, uint8_t *chroma, uint32_t cpitch, uint32_t ch) {
    pthread_mutex_lock(&g_mu);
    unsigned j = 0; while (j < g_nsurf && g_surf[j].luma != src_luma) j++;
    if (j == g_nsurf) { static int k; if (k++ < 6) LOG("B3-MOVIE vic 输入面 %#llx 未知（已知 %u 个）| vic input surface unknown", (unsigned long long)src_luma, g_nsurf); pthread_mutex_unlock(&g_mu); return 0; }
    CVPixelBufferRef p = g_surf[j].pic;
    if (!p) { pthread_mutex_unlock(&g_mu); return 0; }
    CVPixelBufferLockBaseAddress(p, kCVPixelBufferLock_ReadOnly);
    size_t w = CVPixelBufferGetWidthOfPlane(p, 0), h = CVPixelBufferGetHeightOfPlane(p, 0), sp0 = CVPixelBufferGetBytesPerRowOfPlane(p, 0), sp1 = CVPixelBufferGetBytesPerRowOfPlane(p, 1);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(p, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(p, 1);
    size_t cw = w > lpitch ? lpitch : w, chh = (h + 1) / 2;
    for (size_t r = 0; r < lh; r++) memcpy(luma + r * lpitch, y + (r < h ? r : h - 1) * sp0, cw);
    for (size_t r = 0; r < ch; r++) memcpy(chroma + r * cpitch, uv + (r < chh ? r : chh - 1) * sp1, ((w + 1) / 2) * 2 > cpitch ? cpitch : ((w + 1) / 2) * 2);
    CVPixelBufferUnlockBaseAddress(p, kCVPixelBufferLock_ReadOnly);
    pthread_mutex_unlock(&g_mu);
    return 1;
}
