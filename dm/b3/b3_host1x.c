// B3 的 host1x 命令流解析与 VIC / NVDEC 引擎。命令字格式按公开的 host1x 文档（Tegra TRM「Host1x Opcodes」，Linux drivers/gpu/host1x）：
// 位 31–28 是操作码：0 SETCLASS、1 INCR、2 NONINCR、3 MASK、4 IMM；类号 0xF0 = NVDEC、0x5D = VIC。经 THI 接口写方法：寄存器 0x10 = 方法号、0x11 = 数据。
// 方法表与结构布局按 Ryujinx（Ryujinx.Graphics.Nvdec / Vic）的寄存器定义，并用 B3 实际命令流核对。
// B3 host1x command stream parsing and the VIC / NVDEC engines . Command words follow the public host1x docs (Tegra TRM "Host1x Opcodes", Linux drivers/gpu/host1x):
// bits 31–28 are the opcode: 0 SETCLASS, 1 INCR, 2 NONINCR, 3 MASK, 4 IMM; class 0xF0 = NVDEC, 0x5D = VIC. Methods are written through the THI interface: register 0x10 = method id, 0x11 = data.
// Method tables and struct layouts follow Ryujinx's register definitions (Ryujinx.Graphics.Nvdec / Vic), checked against B3's real command streams .
#include "dm.h"

uint64_t b3_nv_iova_addr(uint64_t iova, uint64_t *avail);   // b3_nvhost.c

#define NFD 32
typedef struct { uint32_t cls, mreg, m[0x200]; } chan_t;
static chan_t g_chan[NFD];

static uint64_t addr8(uint32_t v, uint64_t *avail) { return b3_nv_iova_addr((uint64_t)v << 8, avail); }   // 方法值左移 8 位是通道地址 | method value << 8 is the channel address

// VIC 执行（方法 0xC0）：配置结构在方法 0x1c2；输出面亮度 / 色度在 0x1c8 / 0x1c9。输出配置（配置结构 +0x20 起四个字）：
// 字 0 位 0–6 像素格式（68 = Y8___V8U8_N420，NV12）、位 11–14 块类型（0 = 线性）；字 1–3：表面宽高、亮度宽高、色度宽高，各 14 位，值 + 1。
// VIC execute (method 0xC0): config struct at method 0x1c2; output surface luma / chroma at 0x1c8 / 0x1c9. Output config (four words from config struct +0x20):
// word 0 bits 0–6 pixel format (68 = Y8___V8U8_N420, NV12), bits 11–14 block kind (0 = linear); words 1–3: surface, luma and chroma width / height, 14 bits each, value + 1.
typedef struct { uint32_t fmt, kind, lw, lh, cw, ch; uint64_t luma, chroma; } vic_out_t;
static int vic_out(const chan_t *c, vic_out_t *o) {
    uint64_t av = 0; const uint32_t *cfg = (const uint32_t *)(uintptr_t)addr8(c->m[0x1c2], &av);
    if (!cfg || av < 0x30) return 0;
    o->fmt = cfg[8] & 0x7f; o->kind = (cfg[8] >> 11) & 15;
    o->lw = (cfg[10] & 0x3fff) + 1; o->lh = ((cfg[10] >> 14) & 0x3fff) + 1; o->cw = (cfg[11] & 0x3fff) + 1; o->ch = ((cfg[11] >> 14) & 0x3fff) + 1;
    o->luma = addr8(c->m[0x1c8], NULL); o->chroma = addr8(c->m[0x1c9], NULL);
    return o->luma && o->chroma;
}

// 由 b3_movie.c 提供（弱符号：没有时退回测试图样）：把与这个 NVDEC 输出面对应的已解码画面写成 NV12 线性 | provided by b3_movie.c (weak: falls back to a test pattern when absent): write the decoded picture for this NVDEC output surface as linear NV12
__attribute__((weak)) int b3_movie_fill_nv12(uint64_t src_luma_iova, uint8_t *luma, uint32_t lpitch, uint32_t lh, uint8_t *chroma, uint32_t cpitch, uint32_t ch) {
    (void)src_luma_iova; (void)luma; (void)lpitch; (void)lh; (void)chroma; (void)cpitch; (void)ch; return 0; }
__attribute__((weak)) void b3_movie_nvdec(int fd, const uint32_t *m, uint64_t (*addr)(uint32_t, uint64_t *)) { (void)fd; (void)m; (void)addr; }

// nvn_mtl.m（DM_B3）提供：存储在这段内存里的线性贴图重传；单测程序不链 Metal 层，用这个弱定义 | provided by nvn_mtl.m (DM_B3): re-upload linear textures stored in this memory; test programs without the Metal layer use this weak definition
__attribute__((weak)) int b3_tex_refresh_range(uint64_t addr, uint64_t len) { (void)addr; (void)len; return 0; }

static void vic_exec(chan_t *c) {
    vic_out_t o; if (!vic_out(c, &o)) { static int n; if (n++ < 3) LOG("B3-VIC 配置或输出面地址解析失败 | config or output surface address unresolved"); return; }
    if (o.fmt != 68 || o.kind != 0) { static int n; if (n++ < 3) LOG("B3-VIC 输出格式 %u 块类型 %u 未实现 | output format %u block kind %u not implemented", o.fmt, o.kind, o.fmt, o.kind); return; }
    uint8_t *y = (uint8_t *)(uintptr_t)o.luma, *uv = (uint8_t *)(uintptr_t)o.chroma;
    uint32_t lp = o.lw, cp = o.cw * 2;   // 亮度每行 lw 字节；色度每行 cw 对 UV | luma rows are lw bytes; chroma rows are cw UV pairs
    if (b3_movie_fill_nv12((uint64_t)c->m[0x100] << 8, y, lp, o.lh, uv, cp, o.ch)) { b3_tex_refresh_range(o.luma, (uint64_t)lp * o.lh); b3_tex_refresh_range(o.chroma, (uint64_t)cp * o.ch); return; }
    // 没有解码画面：灰色渐变测试图样（先验证 VIC 输出能被游戏显示）| no decoded picture: grey gradient test pattern (first checks that VIC output reaches the screen)
    for (uint32_t r = 0; r < o.lh; r++) memset(y + (size_t)r * lp, (int)(r * 255 / o.lh), lp);
    memset(uv, 128, (size_t)cp * o.ch);
    b3_tex_refresh_range(o.luma, (uint64_t)lp * o.lh); b3_tex_refresh_range(o.chroma, (uint64_t)cp * o.ch);
    static int once; if (!once++) LOG("B3-VIC 首次执行：NV12 线性 亮度 %ux%u 色度 %ux%u（测试图样）| first execute: NV12 linear luma %ux%u chroma %ux%u (test pattern)", o.lw, o.lh, o.cw, o.ch, o.lw, o.lh, o.cw, o.ch);
}

// NVDEC / VIC 在客体线程上执行，那里没有自动释放池；VideoToolbox / Metal 每帧产生的自动释放对象要就地清掉，否则每帧漏一整帧画面（实测约 2 MB / 帧）
// NVDEC / VIC run on guest threads, which have no autorelease pool; the autoreleased objects VideoToolbox / Metal create every frame must be drained here, or each frame leaks a whole picture (measured about 2 MB per frame)
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *pool);

static void method(int fd, chan_t *c, uint32_t reg, uint32_t val) {
    if (reg == 0x10) { c->mreg = val; return; }
    if (reg != 0x11 || c->mreg >= 0x200) return;
    c->m[c->mreg] = val;
    if (c->mreg != 0xC0) return;   // 0xC0 = 执行 | execute
    void *pool = objc_autoreleasePoolPush();
    if (c->cls == 0x5D) vic_exec(c);
    else if (c->cls == 0xF0) b3_movie_nvdec(fd, c->m, addr8);
    objc_autoreleasePoolPop(pool);
}

// 一条命令缓冲：按操作码逐字解析；GATHER 等未用到的操作码记一次后跳过该缓冲余下部分 | one command buffer: parse word by word by opcode; unused opcodes such as GATHER are logged once and the rest of the buffer is skipped
void b3_host1x_cmdbuf(int fd, const uint32_t *w, uint32_t n) {
    if (fd < 0 || fd >= NFD) return;
    chan_t *c = &g_chan[fd];
    for (uint32_t i = 0; i < n; ) {
        uint32_t op = w[i] >> 28, x = w[i++];
        switch (op) {
        case 0: c->cls = (x >> 6) & 0x3ff; { uint32_t off = (x >> 16) & 0xfff, mask = x & 0x3f; for (int b = 0; b < 6; b++) if (mask & (1u << b)) { if (i < n) method(fd, c, off + b, w[i++]); } } break;   // SETCLASS
        case 1: { uint32_t off = (x >> 16) & 0xfff, cnt = x & 0xffff; for (uint32_t k = 0; k < cnt && i < n; k++) method(fd, c, off + k, w[i++]); } break;   // INCR
        case 2: { uint32_t off = (x >> 16) & 0xfff, cnt = x & 0xffff; for (uint32_t k = 0; k < cnt && i < n; k++) method(fd, c, off, w[i++]); } break;       // NONINCR
        case 3: { uint32_t off = (x >> 16) & 0xfff, mask = x & 0xffff; for (int b = 0; b < 16; b++) if (mask & (1u << b)) { if (i < n) method(fd, c, off + b, w[i++]); } } break;   // MASK
        case 4: method(fd, c, (x >> 16) & 0xfff, x & 0xffff); break;   // IMM
        default: { static int once; if (!once++) LOG("B3-HOST1X 未处理的操作码 %u（字 %#x），跳过本缓冲余下部分 | unhandled opcode %u (word %#x), skipping the rest of this buffer", op, x, op, x); } return;
        }
    }
}
