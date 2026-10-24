// NVN 贴图存储布局（Tegra X1 块线性），见 nvn_tex.c。
// NVN texture storage layout (Tegra X1 block-linear), see nvn_tex.c.
#pragma once
#include <stdint.h>

typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / 采样数（0 或 1 = 单采样） | NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    int pkg_block_h;                      // 预排布贴图布局参数第 0 字节（块高 log2）+ 1；0 = 按自动算法 | Byte 0 of the packaged texture layout parameters (log2 of block height) + 1; 0 = use the automatic algorithm
#ifdef DM_B3
    int pkg_tile_w;                       // B3：布局参数第 1 字节（块宽 log2，GOB 数）+ 1；行宽按 2^块宽 个 GOB 对齐；0 = 不对齐 | B3 : byte 1 of the layout parameters (log2 of block width in GOBs) + 1; the row pitch is aligned to 2^width GOBs; 0 = no alignment
#endif
} nvn_texdesc;

#ifdef DM_B3
// B3：B3 的 NVNtexture 对象间距是 128 字节（实测：贴图对象数组里第 128 字节起就是下一个对象的 magic），tex_t 必须 ≤ 128，
// 否则 serial 落在下一个对象里被覆盖，几张环境立方体贴图共用一个假序号、绑成同一张宿主贴图（菜单人物着色错）。这里把块高 / 块深缩成 1 字节。
// B3 : B3's NVNtexture objects are 128 bytes apart (measured: in an array of texture objects, byte 128 onward is the next object's magic), so tex_t must be <= 128;
// otherwise serial lands in the next object and gets overwritten, and several environment cube maps share one bogus serial and bind to the same host texture (menu character shading wrong). Block height / depth shrink to one byte here.
typedef struct {
    uint64_t size, layer_size;            // 总存储字节、每层字节 | Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // 对齐、层数、每块字节 | Alignment, layer count, bytes per block
    uint8_t tileH, tileD;                 // 块高 / 块深（log2 GOB 数）；B3 用 1 字节，让 tex_t 不超过 128 字节 | Block height / block depth (log2 of the GOB count); one byte in B3 so tex_t stays within 128 bytes
} nvn_texlayout;
#else
typedef struct {
    uint64_t size, layer_size;            // 总存储字节、每层字节 | Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // 对齐、层数、每块字节 | Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // 块高 / 块深（log2 GOB 数） | Block height / block depth (log2 of the GOB count)
} nvn_texlayout;
#endif

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
int nvn_fmt_bpb(int format, int *bw, int *bh);   // 每块字节（0 = 未知格式），块宽高 | Bytes per block (0 = unknown format), block width and height
// 第 level 级实际使用的块高 / 块深（log2 GOB 数）：贴图级块高按该级行数（块行）收缩，解排布逐级用它
// Block height / block depth actually used by level `level` (log2 of the GOB count): the per-level block height shrinks with that level's row count (block rows), and deswizzling uses it level by level
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
#ifdef DM_B3
uint32_t nvn_b3_pitch_gobs(const nvn_texdesc *d, uint32_t wg);   // nvn_tex.c：带块宽对齐的行宽（GOB 数）| nvn_tex.c: row pitch in GOBs with block-width alignment
#endif
