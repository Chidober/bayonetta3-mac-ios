// B3 补的 NVN 函数：通用层的接管层（dm/gfx/nvn）没实现、B3 实际调到的那些，按 NVN 公开头文件的签名写在这里，不改通用层的 hle_nvn.c。
// 登记方式与 hle_nvn.c 相同：条目放进 __DATA,swnvn 段，nvn_init_slots 启动时按名字装进槽位；对象布局用通用层的 nvn_obj.h（缓冲是 B3 新加的，通用层不用 NVNbuffer）。
// 地址约定沿用通用层：GPU 地址 = 宿主指针（nvnMemoryPoolGetBufferAddress 返回池存储首地址），缓冲地址 = 池地址 + 偏移。
// NVN functions added for B3: the ones the base runtime's takeover layer (dm/gfx/nvn) does not implement and B3 actually calls, written here from the public NVN header signatures without touching the base runtime's hle_nvn.c.
// Registration matches hle_nvn.c: entries go into the __DATA,swnvn section and nvn_init_slots installs them into slots by name at start-up; object layouts come from the base runtime's nvn_obj.h (buffers are new for B3; the base runtime does not use NVNbuffer).
// Address convention follows the base runtime: GPU address = host pointer (nvnMemoryPoolGetBufferAddress returns the pool storage start), buffer address = pool address + offset.
#include <pthread.h>
#include "dm.h"
#include <sys/stat.h>
static int b3_diag(void) { static int on = -1; if (on < 0) { const char *e = getenv("B3_DIAG_LOG"); on = e && *e == '1'; } return on; }   // B3_DIAG_LOG=1：逐条诊断日志（缺省关）| B3_DIAG_LOG=1: per-call diagnostic logs (off by default)
#include "../gfx/nvn/nvn_protos.h"
#include "../gfx/nvn/nvn_obj.h"

typedef struct { const char *name; void *fn; } nvn_impl;   // 与 hle_nvn.c 的条目布局一致 | same entry layout as hle_nvn.c
#define NVN(name, ret, ...)                                                                       \
    static ret b3_##name(__VA_ARGS__);                                                            \
    static const NVNT_##name chk_##name __attribute__((unused)) = b3_##name;                      \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)b3_##name}; \
    static ret b3_##name(__VA_ARGS__)
// 生成的原型表把「返回 NVNmemoryPool *」写成了按值返回的几个函数：不做签名检查
// The few functions the generated prototype table lists as returning NVNmemoryPool by value instead of a pointer: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret b3_##name(__VA_ARGS__);                                                            \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)b3_##name}; \
    static ret b3_##name(__VA_ARGS__)

// 取通用层已登记的实现（hle_nvn.c 的 swnvn 条目），用来把 B3 的变体接到通用层的同类命令上 | Fetch the base runtime's registered implementation (hle_nvn.c's swnvn entry) so B3's variants can reuse the base runtime's commands
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
static void *base_impl(const char *name) {
    unsigned long sz; const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    for (unsigned long k = 0; im && k < sz / sizeof *im; k++) if (!strcmp(im[k].name, name)) return im[k].fn;
    die("B3 NVN：通用层实现 %s 不在 swnvn 段 | base runtime implementation not in the swnvn section", name);
}
#define BASE(name) ((NVNT_##name)base_impl(#name))

// ---------- 快速清除色登记：只是压缩提示，宿主没有对应物；返回「已登记」 ----------
// ---------- Fast-clear registration: only a compression hint with no host equivalent; return "registered" ----------
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
NVN(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *d, const int *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
NVN(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *d, const uint32_t *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float z) { (void)d; (void)z; return 1; }

// ---------- 内存池查询 ----------
// ---------- Memory pool queries ----------
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return ((const pool_t *)p)->mem; }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }   // CPU 与「GPU」同一份内存 | CPU and "GPU" share one memory
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return ((const pool_t *)p)->size; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }

// ---------- 贴图尺寸 ----------
// ---------- Texture sizes ----------
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { tex_builder_t *t = (tex_builder_t *)b; t->d.width = w; t->d.height = h; t->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int z) { tex_builder_t *t = (tex_builder_t *)b; t->d.width = w; t->d.height = h; t->d.depth = z; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)((const tex_t *)t)->d.target; }
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) {   // 深度 / 模板采样选择：宿主按视图格式读，不存（DECISION） | depth / stencil sampling choice: the host reads by view format, not stored (DECISION)
    (void)b; static int once; if (m && !once++) LOG("B3-NVN-NOTE nvnTextureBuilderSetDepthStencilMode mode=%d 未存 | not stored", (int)m);
}
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { (void)b; return 0; }   // 宿主没有 ZCull，存档区大小 0（HYPOTHESIS：游戏接受 0） | the host has no ZCull; save area size 0 (HYPOTHESIS: the game accepts 0)
NVN_NOPROTO(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)((const tex_t *)t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return ((const tex_t *)t)->off; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)((const tex_t *)t)->d.format; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return (int)((const tex_t *)t)->d.samples; }   // 建贴图时 nvnTextureBuilderSetSamples 存的值原样返回（非多重采样为 0）| returns the value nvnTextureBuilderSetSamples stored at creation (0 when not multisampled)

// CPU 写贴图：步长默认紧排（与通用层的 read_texels 相同算法），写入由 Metal 层 mtl_write_texels 完成
// CPU texture write: strides default to tightly packed (same rule as the base runtime's read_texels); the write itself is done by the Metal layer's mtl_write_texels
static void write_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    mtl_write_texels(o, level, layer0, r, src, (size_t)row, (size_t)img);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) {
    write_texels((const tex_t *)t, (const view_t *)v, r, src, 0, 0);
}
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels((const tex_t *)t, (const view_t *)v, r, src, row, img);
}
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    // 通道重排：存进 builder（DM_B3 字段），TextureInitialize 抄进贴图，Metal 层建贴图时设 swizzle
    // Channel swizzle: stored in the builder (DM_B3 field), copied into the texture by TextureInitialize, set as the Metal texture's swizzle by the Metal layer 
    tex_builder_t *o = (tex_builder_t *)b; o->b3_swz[0] = (uint8_t)r; o->b3_swz[1] = (uint8_t)g; o->b3_swz[2] = (uint8_t)bl; o->b3_swz[3] = (uint8_t)a;
}
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) {
    // 1 = 采样模板而不是深度：Metal 层给这种视图建 X32_Stencil8 格式（角色被模板 0x80 标记，后续全屏通道按模板分类着色）
    // 1 = sample stencil instead of depth: the Metal layer builds such views as X32_Stencil8 (characters are marked with stencil 0x80, and later fullscreen passes classify shading by stencil)
    ((view_t *)v)->b3_ds = (int32_t)m;
}

// ---------- 颜色状态 ----------
// ---------- Color state ----------
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) {
    // 存进通用层颜色状态已有的 alpha_func 位；回放层（DM_B3）据此选片元着色器的 alpha 测试版本
    // Stored in the alpha_func bits that the base runtime's color state already has; the replay layer (DM_B3) uses it to pick the fragment shader's alpha test variant 
    ((color_st *)s)->alpha_func = (uint32_t)f & 0xF;

}
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)((const color_st *)s)->alpha_func; }

// ---------- 命令缓冲：B3 用到的变体，录成通用层已有的命令 ----------
// ---------- Command buffers: variants B3 uses, recorded as commands the base runtime already has ----------
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) {   // 单个视口 = SetViewports(0, 1) | one viewport = SetViewports(0, 1)
    static NVNT_nvnCommandBufferSetViewports f; if (!f) f = BASE(nvnCommandBufferSetViewports);
    const float v[4] = {(float)x, (float)y, (float)w, (float)h}; f(c, 0, 1, v);
}
// 填缓冲：按 GPU 时间线写入，录成若干条 UpdateUniformBuffer（内嵌数据，与其他命令同序执行）
// Fill a buffer: written on the GPU timeline, recorded as UpdateUniformBuffer records (inline data, executed in order with the other commands)
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t value) {
    static NVNT_nvnCommandBufferUpdateUniformBuffer f; if (!f) f = BASE(nvnCommandBufferUpdateUniformBuffer);
    enum { CHUNK = 0x4000 }; uint32_t pat[CHUNK / 4]; for (unsigned i = 0; i < CHUNK / 4; i++) pat[i] = value;
    for (size_t off = 0; off < n; off += CHUNK) f(c, a, n, (ptrdiff_t)off, n - off < CHUNK ? n - off : CHUNK, pat);
}
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) {   // Metal 只有 1 像素线 | Metal only draws 1-pixel lines
    (void)c; static int once; if (w != 1.0f && !once++) LOG("B3-NVN-NOTE nvnCommandBufferSetLineWidth %g 按 1 画 | drawn as 1", w);
}
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }   // 分块缓存提示，宿主无对应物 | tiled cache hint, no host equivalent
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }   // 颜色压缩提示 | colour compression hint
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int fmt_class, int conservative, int aggressive) { (void)c; (void)fmt_class; (void)conservative; (void)aggressive; }
// 队列等同步对象：通用层只有一个按提交顺序执行的队列，同步对象围栏在它之前的提交上，等待自然满足；返回成功
// Queue waits on a sync object: the base runtime has a single queue executing in submit order and the sync was fenced on an earlier submit, so the wait is satisfied by ordering; return success
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { (void)q; (void)s; return 1; }
// 图像描述符：通用层的图像句柄 = 描述符号，与贴图登记共用同一张表 | Image descriptors: the base runtime's image handle = descriptor index, sharing the texture registration table
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *p, int id, const NVNtexture *t, const NVNtextureView *v) {
    static NVNT_nvnTexturePoolRegisterTexture f; if (!f) f = BASE(nvnTexturePoolRegisterTexture); f(p, id, t, v);
}

// ---------- 着色器头部元数据（的输入）----------
// 每个不同的 control 段（通用层按其前 0x880 字节的 SHA-256 作键）记一行：程序序号、阶段槽位、哈希前 8 字节、control 段前 8 个 u32（头部元数据，含义待 核实，HYPOTHESIS）。不记着色器代码字节。
// ---------- Shader header metadata (input for ) ----------
// One line per distinct control section (the base runtime keys it by the SHA-256 of its first 0x880 bytes): program serial, stage slot, first 8 bytes of the hash, the first 8 u32 of the control section (header metadata, meaning to be verified in , HYPOTHESIS). No shader code bytes are logged.
#include <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
// 程序序号 → 各阶段控制段键（SetShaders 时算好）：NVN 允许 SetShaders 之后释放控制段内存，回放层到第一次绘制才算键会算到被改写的内存（实测 11 个对不上的键），所以 B3 下回放层改用这里记下的键
// Program serial → per-stage control section keys (computed at SetShaders): NVN allows the control section memory to be freed after SetShaders, and the replay layer hashing at the first draw reads overwritten memory (11 mismatched keys measured in ), so under B3 the replay layer uses the keys recorded here
// 开放寻址表，按序号找，不淘汰（原先按 序号 % 32768 直接覆盖，进 3D 后合成程序不断拿新序号，把普通程序的键挤掉，回放层退回去散列已改写的控制段，算出不存在的键）。
// 普通程序与合成程序共用；同一序号再写就覆盖。表满（探测 64 次没空位）记红一次，那个序号的键就查不到（回放层按缺着色器处理）。
// Open-addressed table looked up by serial, no eviction (the old serial % 32768 slots were overwritten as composites kept taking new serials in 3D, evicting plain programs' keys; the replay layer then fell back to hashing overwritten control sections and got keys that do not exist).
// Shared by plain and composite programs; writing the same serial again overwrites. When full (64 probes without a free slot) one red line is logged and that serial's keys cannot be found (the replay layer treats it as a missing shader).
enum { PKEYS = 1 << 20 };
typedef struct { uint64_t serial; uint64_t key[6]; } pkey_t;
static pkey_t *g_pkey;
static pthread_mutex_t g_pkey_lock = PTHREAD_MUTEX_INITIALIZER;
static pkey_t *pkey_slot(uint64_t serial, int insert) {   // 调用方持锁 | caller holds the lock
    if (!g_pkey && !(g_pkey = calloc(PKEYS, sizeof *g_pkey))) die("B3 NVN：键表内存不足 | out of memory for the key table");
    uint64_t h = serial * 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < 64; i++) { pkey_t *e = &g_pkey[(h + i) % PKEYS]; if (e->serial == serial) return e; if (!e->serial) return insert ? e : NULL; }
    if (insert) { static int once; if (!once++) LOG("【红】B3 程序键表满 | program key table full"); }
    return NULL;
}
int b3_prog_key(uint64_t serial, int stage, char out[17]) {
    pthread_mutex_lock(&g_pkey_lock);
    pkey_t *e = serial ? pkey_slot(serial, 0) : NULL;
    int ok = e && stage >= 0 && stage < 6 && e->key[stage];
    if (ok) snprintf(out, 17, "%016llx", (unsigned long long)e->key[stage]);
    pthread_mutex_unlock(&g_pkey_lock);
    return ok;
}
// 按阶段绑定的程序：B3 每个阶段绑一个单独的程序（BindProgram 的阶段位只覆盖该阶段），通用层的回放层只认一个当前程序。
// 回放层（nvn_mtl.m，DM_B3）每次 BindProgram 调这里：记下每个阶段当前的程序；只有一个不同的程序时原样返回，否则返回由各程序拼成的合成程序（按组成程序的序号缓存，自带序号与键）。
// HYPOTHESIS：一个程序被绑到某些阶段时，它的全部阶段都放进合成程序，后绑的阶段（按阶段号顺序）覆盖前面的。
// Programs bound per stage : B3 binds a separate program for each stage (BindProgram's stage bits cover only that stage), while the base runtime's replay layer knows a single current program.
// The replay layer (nvn_mtl.m, DM_B3) calls this on every BindProgram: it records each stage's current program; with only one distinct program it returns it unchanged, otherwise a composite program built from them (cached by the component serials, with its own serial and keys).
// HYPOTHESIS: when a program is bound to some stages, all its stages go into the composite, and later stages (in stage order) override earlier ones.
uint64_t nvn_new_serial(void);
// 计算阶段（阶段 5）单独记、不进图形合成程序（VERIFIED：游戏先绑计算程序再绑顶点 / 像素时，旧做法把计算着色器也拼进图形程序）；b3_prog_compute 回当前计算程序。
// The compute stage (stage 5) is tracked separately and kept out of graphics composites (VERIFIED: when the game binds a compute program and then vertex / pixel ones, the old code put the compute shader into the graphics program); b3_prog_compute returns the current compute program.
static const prog_t *g_b3_cur_cs;
const prog_t *b3_prog_compute(void) { return g_b3_cur_cs; }
const prog_t *b3_prog_bind(const prog_t *p, int stages) {
    static const prog_t *cur[5];
    if (stages & (1 << 5)) g_b3_cur_cs = p;
    for (int st = 0; st < 5; st++) if (stages & (1 << st)) cur[st] = p;
    const prog_t *parts[5]; int np = 0;
    for (int st = 0; st < 5; st++) { int dup = 0; if (!cur[st]) continue; for (int j = 0; j < np; j++) if (parts[j] == cur[st]) dup = 1; if (!dup) parts[np++] = cur[st]; }
    if (np == 0) return NULL;
    if (np == 1) return parts[0];
    // 合成缓存：探测 8 个槽，都占了就替换第一个（原 4096 个直接映射槽在 3D 里来回替换，每次替换都拿新序号、重建管线）
    // Composite cache: probe 8 slots and replace the first when all are taken (the old 4096 direct-mapped slots kept replacing each other in 3D, each replacement taking a new serial and rebuilding pipelines)
    enum { NC = 1 << 16, PROBE = 8 }; static struct { uint64_t ser[6]; int n; prog_t *c; } cache[NC];
    uint64_t h = 1469598103934665603ull; for (int j = 0; j < np; j++) h = (h ^ parts[j]->serial) * 1099511628211ull;
    unsigned slot = (unsigned)(h % NC);
    for (unsigned q = 0; q < PROBE; q++) {
        unsigned sq = (slot + q) % NC;
        if (!cache[sq].c) { slot = sq; break; }
        int hit = cache[sq].n == np; for (int j = 0; hit && j < np; j++) hit = cache[sq].ser[j] == parts[j]->serial;
        if (hit) return cache[sq].c;
    }
    prog_t *c = cache[slot].c ? cache[slot].c : calloc(1, sizeof(prog_t));   // 槽被换掉时复用内存：旧合成程序的序号作废，回放层按新序号重新解析 | memory is reused when a slot is replaced: the old composite's serial is dropped and the replay layer resolves the new serial afresh
    memset(c, 0, sizeof *c); c->magic = parts[0]->magic; c->serial = nvn_new_serial();
    uint64_t keys[6] = {0};
    for (int j = 0; j < np; j++) for (unsigned i = 0; i < parts[j]->nstage && c->nstage < 6; i++) {
        char kb[17]; int b3_prog_key(uint64_t serial, int stage, char out[17]);
        c->sd[c->nstage] = parts[j]->sd[i]; keys[c->nstage] = b3_prog_key(parts[j]->serial, (int)i, kb) ? strtoull(kb, NULL, 16) : 0; c->nstage++;
    }
    cache[slot].c = c; cache[slot].n = np; for (int j = 0; j < np; j++) cache[slot].ser[j] = parts[j]->serial;
    extern void b3_prog_key_put(uint64_t serial, const uint64_t keys[6]); b3_prog_key_put(c->serial, keys);
    return c;
}
void b3_prog_key_put(uint64_t serial, const uint64_t keys[6]) {
    pthread_mutex_lock(&g_pkey_lock); pkey_t *e = pkey_slot(serial, 1); if (e) { e->serial = serial; memcpy(e->key, keys, 6 * sizeof *keys); } pthread_mutex_unlock(&g_pkey_lock);
}
void b3_nvn_shader_note(const NVNshaderData *sd, int n, uint64_t serial) {
    enum { SEEN = 8192 }; static uint64_t seen[SEEN]; static int nseen;
    { uint64_t keys[6] = {0};
      for (int i = 0; i < n && i < 6; i++) if (sd[i].control) { uint8_t md[32]; CC_SHA256(sd[i].control, 0x880, md); uint64_t k; memcpy(&k, md, 8); keys[i] = __builtin_bswap64(k); }
      b3_prog_key_put(serial, keys); }
    for (int i = 0; i < n; i++) {
        if (!sd[i].control) continue;
        uint8_t md[32]; CC_SHA256(sd[i].control, 0x880, md); uint64_t key; memcpy(&key, md, 8);
        int dup = 0; for (int k = 0; k < nseen; k++) if (seen[k] == key) { dup = 1; break; }
        if (dup) continue;
        if (nseen < SEEN) seen[nseen++] = key;
        const uint32_t *w = (const uint32_t *)sd[i].control;
        // B3_SHADER_DUMP=<目录>：每个新键写 <键>.ctl（控制段，长度取头部第 5 字，上限 0x10000）、<键>.code（代码区 + 0x40 余量）并往 shader_map.tsv 追加一行；只写到宿主目录，不进仓库
        // B3_SHADER_DUMP=<dir>: for each new key write <key>.ctl (the control section, length from header word 5, capped at 0x10000), <key>.code (code area + 0x40 margin) and append a row to shader_map.tsv; host directory only, never the repository
        { const char *dd = getenv("B3_SHADER_DUMP");
          if (dd && *dd) { char path[1200]; uint32_t cl = w[5] && w[5] <= 0x10000 ? w[5] : 0x880; FILE *fp;
              snprintf(path, sizeof path, "%s/%016llx.ctl", dd, (unsigned long long)__builtin_bswap64(key)); if ((fp = fopen(path, "wb"))) { fwrite(sd[i].control, 1, cl, fp); fclose(fp); }
              // 映射表行（与通用层 shader_map.tsv 前四列同格式：包、序号、阶段、键）：代码头 0x12345679 = 计算；否则 +0x30 处 Maxwell 程序头第 0 字的位 10–13 是阶段（Ryujinx 编号：1 顶点、2 外壳、3 域、4 几何、5 像素）
              // Map row (same format as the first four columns of the base runtime's shader_map.tsv: package, index, stage, key): code header 0x12345679 = compute; otherwise bits 10–13 of word 0 of the Maxwell program header at +0x30 give the stage (Ryujinx numbering: 1 vertex, 2 hull, 3 domain, 4 geometry, 5 pixel)
              if (sd[i].data) { const uint32_t *cw = (const uint32_t *)(uintptr_t)sd[i].data; static const char *zh[6] = {"计算", "顶点", "外壳", "域", "几何", "像素"};
                  const char *stg = cw[0] == 0x12345679 ? zh[0] : cw[0] == 0x12345678 && ((cw[12] >> 10) & 15) >= 1 && ((cw[12] >> 10) & 15) <= 5 ? zh[(cw[12] >> 10) & 15] : NULL;
                  snprintf(path, sizeof path, "%s/shader_map.tsv", dd);
                  if (stg && (fp = fopen(path, "a"))) { fprintf(fp, "b3\t%llu\t%s\t%016llx\n", (unsigned long long)serial, stg, (unsigned long long)__builtin_bswap64(key)); fclose(fp); }
                  else if (!stg) LOG("B3-NVN-SHADER 阶段不明 ctl=%016llx code0=%08x | stage unknown", (unsigned long long)__builtin_bswap64(key), cw[0]); }
              // 计算着色器（代码头 0x12345679）的指令从 +0x100 起（0x30 头后补 0 到 0x100，三个样本的结尾都落在 0x100 + 长度内）| compute shaders (code header 0x12345679) start their instructions at +0x100 (the 0x30 header is zero-padded to 0x100; the end fell within 0x100 + length in all three samples)
              uint32_t code_len = cl > 0x6fc ? (sd[i].data && *(const uint32_t *)(uintptr_t)sd[i].data == 0x12345679 ? 0x100 : 0x30) + w[0x6f8 / 4] : 0;   // HYPOTHESIS：代码区长 = 0x30 头 + 控制段 +0x6f8 的值（用 5 个样本对上 +0x700 的 256 对齐值）| code area length = 0x30 header + control +0x6f8 (matched the 256-aligned +0x700 on 5 samples in )
              // c1（编译器生成的常量，着色器读 c[1]）：在代码区 +控制段 0x700（代码区按 256 对齐后的长度）处，长度为控制段 +0x6fc（一个像素着色器 12 字节 = 0.1、1/3、1/255）
              // c1 (compiler-generated constants the shader reads as c[1]): at code + control 0x700 (the code area length aligned to 256), length control +0x6fc (12 bytes in a pixel shader = 0.1, 1/3, 1/255)
              { uint32_t c1len = cl > 0x704 ? w[0x6fc / 4] : 0, c1off = cl > 0x704 ? w[0x700 / 4] : 0;
                snprintf(path, sizeof path, "%s/%016llx.c1", dd, (unsigned long long)__builtin_bswap64(key));
                if (sd[i].data && c1len && c1len <= 0x10000 && c1off <= 0x100000 && (fp = fopen(path, "wb"))) { fwrite((const uint8_t *)(uintptr_t)sd[i].data + c1off, 1, c1len, fp); fclose(fp); } }
              snprintf(path, sizeof path, "%s/%016llx.code", dd, (unsigned long long)__builtin_bswap64(key)); if (sd[i].data && code_len && code_len <= 0x100000 && (fp = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)sd[i].data, 1, code_len + 0x40, fp); fclose(fp); } } }
        int64_t gap = (int64_t)((uint64_t)(uintptr_t)sd[i].control - (uint64_t)sd[i].data);   // 控制段相对代码起点的距离（同一记录里紧邻时 = 代码区长）| distance from the code start to the control section (= code area length when they are adjacent in one record)
        if (b3_diag()) LOG("B3-NVN-SHADER prog=%llu slot=%d n=%d ctl=%016llx gap=%lld head=[%08x %08x %08x %08x %08x %08x %08x %08x]", (unsigned long long)serial, i, n,
            (unsigned long long)__builtin_bswap64(key), (long long)gap, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

// ---------- CPU 写监视（诊断，B3_WATCH_W）----------
// 被监视页设只读；写入触发 SIGBUS/SIGSEGV 时记下 PC 与地址（落在监视区的才记），放开该页让写完成；后台线程每 50 µs 重新设只读。只用于诊断，可能漏掉同一窗口内的连续写。
// ---------- CPU write watch (diagnostic, B3_WATCH_W) ----------
// The watched page is made read-only; a write raises SIGBUS/SIGSEGV, the handler records the PC and address (only inside the watched ranges) and unprotects the page so the write completes; a background thread re-protects it every 50 µs. Diagnostic only; consecutive writes within one window may be missed.
#include <signal.h>
#include <sys/ucontext.h>
static uint64_t g_w_a[2], g_w_page[2]; static long g_w_pg; static struct sigaction g_w_old_bus, g_w_old_segv; static _Atomic int g_w_hits;
static void w_handler(int sig, siginfo_t *si, void *uc) {
    uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
    for (int i = 0; i < 2; i++) if (g_w_page[i] && a >= g_w_page[i] && a < g_w_page[i] + (uint64_t)g_w_pg) {
        mprotect((void *)(uintptr_t)g_w_page[i], (size_t)g_w_pg, PROT_READ | PROT_WRITE);
        for (int j = 0; j < 2; j++) if (g_w_a[j] && a >= g_w_a[j] && a < g_w_a[j] + 32 && g_w_hits < 200) {
            g_w_hits++; uint64_t pc = ((ucontext_t *)uc)->uc_mcontext->__ss.__pc, lr = ((ucontext_t *)uc)->uc_mcontext->__ss.__lr;
            char b[160]; int n = snprintf(b, sizeof b, "[dm] B3-WATCH-HIT addr=%#llx pc=%#llx lr=%#llx\n", (unsigned long long)a, (unsigned long long)pc, (unsigned long long)lr); write(2, b, (size_t)n);
        }
        return;
    }
    struct sigaction *o = sig == SIGBUS ? &g_w_old_bus : &g_w_old_segv;
    if (o->sa_flags & SA_SIGINFO) o->sa_sigaction(sig, si, uc); else if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) o->sa_handler(sig); else { signal(sig, SIG_DFL); raise(sig); }
}
static void *w_thread(void *x) { (void)x; for (;;) { for (int i = 0; i < 2; i++) if (g_w_page[i]) mprotect((void *)(uintptr_t)g_w_page[i], (size_t)g_w_pg, PROT_READ); usleep(50); } return NULL; }
void b3_watch_start(uint64_t a0, uint64_t a1) {
    g_w_pg = sysconf(_SC_PAGESIZE); g_w_a[0] = a0; g_w_a[1] = a1;
    g_w_page[0] = a0 & ~(uint64_t)(g_w_pg - 1); g_w_page[1] = a1 ? a1 & ~(uint64_t)(g_w_pg - 1) : 0; if (g_w_page[1] == g_w_page[0]) g_w_page[1] = 0;
    struct sigaction sa = {0}; sa.sa_sigaction = w_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &g_w_old_bus); sigaction(SIGSEGV, &sa, &g_w_old_segv);
    pthread_t t; pthread_create(&t, NULL, w_thread, NULL); pthread_detach(t);
    LOG("B3-WATCH-W start a0=%#llx a1=%#llx page=%ld", (unsigned long long)a0, (unsigned long long)a1, g_w_pg);
}

// ---------- 截帧：B3_CAPTURE_DIR 设了时，按 B3_CAPTURE_FRAMES（逗号分隔的出图序号，缺省 "1"）把交换链贴图读回，写 PPM 并记一行像素统计 ----------
// 只认 RGBA8 / BGRA8 一类 4 字节格式；统计：尺寸、全黑比例（三通道都 < 8）、平均色、出现最多的颜色（每通道取高 4 位分桶）。图片只留在宿主目录，不进仓库。
// ---------- Frame capture : when B3_CAPTURE_DIR is set, read back the swapchain texture for the present numbers in B3_CAPTURE_FRAMES (comma-separated, default "1"), write a PPM and log one line of pixel statistics ----------
// Only 4-byte formats such as RGBA8 / BGRA8; statistics: size, black fraction (all three channels < 8), mean colour, most frequent colour (bucketed by the top 4 bits of each channel). Images stay in the host directory, never in the repository.
__attribute__((weak)) void b3_snap_flush(const char *dir) { (void)dir; }   // nvn_mtl.m 提供；单测程序不链 Metal 层 | provided by nvn_mtl.m; test programs do not link the Metal layer
_Atomic uint64_t g_b3_dump_at;   // 窗口热键 F12 请求抓的帧号（0 = 无）：该帧做通道记录与通道快照| frame number requested by the window hotkey F12 (0 = none): pass dump and pass snapshots for that frame 
_Atomic uint64_t g_b3_frame_now;   // 最近一次出图的帧号 | frame number of the latest present
static int capture_wanted(uint64_t n) {
    { uint64_t d = atomic_load(&g_b3_dump_at); if (d && n == d) return 1; }
    const char *f = getenv("B3_CAPTURE_FRAMES"); if (!f || !*f) f = "1";
    for (const char *p = f; *p; ) { char *e; unsigned long v = strtoul(p, &e, 10); if (e == p) break; if (v == n) return 1; p = *e == ',' ? e + 1 : e; }
    return 0;
}
_Atomic uint64_t g_b3_presents;   // 出图次数（窗口标题算帧率用，b3_front_mac.m）| present count (used by b3_front_mac.m for the frame rate in the window title)
// 菜单 / 战斗识别（抓的 8 种界面）：最后一个深度通道之后的绘制数——标题 82、初始菜单 71、章节选择 393、战斗练习 173、暂停 164、角色菜单 205；
// 过场与实机都是 1。≥ 20 持续 15 帧判菜单，≤ 5 持续 15 帧判战斗（标题画面实测低到 41），中间保持（防抖）。图形层不在时（单测）恒为战斗
// Menu / combat detection (8 screen types grabbed by ): draws after the last depth pass -- title 82, first menu 71, chapter select 393, battle practice 173, pause 164, character menu 205;
// cutscenes and gameplay are both 1. ≥ 20 for 15 frames means menu, ≤ 5 for 15 frames means combat (the title screen measured as low as 41), in between keeps the state (debounce). Without the graphics layer (unit tests) it is always combat
__attribute__((weak)) _Atomic int g_b3_ui_draws;
static _Atomic int g_b3_menu;
int b3_ui_menu(void) { return atomic_load(&g_b3_menu); }
static void ui_mode_tick(uint64_t n) {
    static int run; int d = atomic_load(&g_b3_ui_draws), m = atomic_load(&g_b3_menu);
    int want = d >= 20 ? 1 : d <= 5 ? 0 : m;
    run = want != m ? run + 1 : 0;
    if (run >= 15) { atomic_store(&g_b3_menu, want); run = 0; LOG("B3-UIMODE %s 帧 %llu 界面绘制 %d | %s at frame %llu, UI draws %d", want ? "菜单" : "战斗", (unsigned long long)n, d, want ? "menu" : "combat", (unsigned long long)n, d); }
}
void b3_present_note(const tex_t *t, uint64_t n, const int crop[4]) {
    atomic_fetch_add(&g_b3_presents, 1); atomic_store(&g_b3_frame_now, n); ui_mode_tick(n);
    { static int once; if (!once++) { const char *g = getenv("B3_GRAB"); if (g && *g) atomic_store(&g_b3_dump_at, strtoull(g, NULL, 10)); } }   // B3_GRAB=帧号：不开窗口时按帧号抓（同 F12）| B3_GRAB=frame: grab by frame number without the window (same as F12)
    { uint64_t d = atomic_load(&g_b3_dump_at); if (d && n == d + 2) { const char *cd = getenv("B3_CAPTURE_DIR"); char def[1024];
          if (!cd || !*cd) { snprintf(def, sizeof def, "%s/b3-work/play/cap", getenv("HOME") ? getenv("HOME") : "."); cd = def; }
          char sub[1100]; snprintf(sub, sizeof sub, "%s/grab_%llu", cd, (unsigned long long)d); mkdir(cd, 0755); mkdir(sub, 0755); b3_snap_flush(sub); } }   // 回放比出图记号晚，等两帧再存 | replay lags the present note, so save two frames later
    { const char *wr = getenv("B3_WATCH_W");   // B3_WATCH_W=地址[,地址]@帧：从该帧起持续监视这两处各 32 字节的 CPU 写，记下写入的 PC（诊断）| B3_WATCH_W=addr[,addr]@frame: from that frame on, watch CPU writes to 32 bytes at each address and log the writing PC (diagnostic)
      if (wr && *wr) { char *e; uint64_t a0 = strtoull(wr, &e, 0), a1 = *e == ',' ? strtoull(e + 1, &e, 0) : 0; uint64_t f = *e == '@' ? strtoull(e + 1, NULL, 0) : 1;
          if (a0 && n == f) { extern void b3_watch_start(uint64_t a0, uint64_t a1); b3_watch_start(a0, a1); } } }
    { const char *wr = getenv("B3_WATCH_RO");   // B3_WATCH_RO=地址@帧：到该帧把地址所在页设成只读，之后第一次 CPU 写会以 CRASH 行报出写入点与地址（诊断用）| B3_WATCH_RO=addr@frame: at that frame make the page read-only; the next CPU write shows up as a CRASH line with the writer and address (diagnostic)
      if (wr && *wr) { char *e; uint64_t a = strtoull(wr, &e, 0); uint64_t f = *e == '@' ? strtoull(e + 1, NULL, 0) : 1;
          if (a && n == f) { long pg = sysconf(_SC_PAGESIZE); uint64_t base = a & ~(uint64_t)(pg - 1);
              int r = mprotect((void *)(uintptr_t)base, (size_t)pg, PROT_READ); LOG("B3-WATCH-RO frame=%llu page=%#llx size=%ld r=%d", (unsigned long long)n, (unsigned long long)base, pg, r); } } }
    { const char *pf = getenv("B3_FILL32");   // B3_FILL32=地址:字节数:值：每次出图把这段内存填成该 u32（诊断）| B3_FILL32=addr:bytes:value: fill this memory with the u32 on every present (diagnostic)
      if (pf && *pf) { char *e; uint64_t a = strtoull(pf, &e, 0), len = *e == ':' ? strtoull(e + 1, &e, 0) : 0; uint32_t v = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 0) : 0;
          uint32_t *w = (uint32_t *)(uintptr_t)a; for (uint64_t i = 0; a && i < len / 4; i++) w[i] = v; } }
    { const char *pz = getenv("B3_PEEKNZ");   // B3_PEEKNZ=地址:字节数：在截帧帧数记下这段内存里非零 u32 的个数与前几个位置（诊断）| B3_PEEKNZ=addr:bytes: on capture frames log the number of non-zero u32 in this memory and the first few positions (diagnostic)
      if (pz && *pz && capture_wanted(n)) { char *e; uint64_t a = strtoull(pz, &e, 0); uint64_t len = *e == ':' ? strtoull(e + 1, NULL, 0) : 4096; const uint32_t *w = (const uint32_t *)(uintptr_t)a; uint64_t nz = 0; char first[256] = ""; size_t o = 0;
          for (uint64_t i = 0; a && i < len / 4; i++) if (w[i]) { if (nz < 8) o += snprintf(first + o, sizeof first - o, " [%llu]=%#x", (unsigned long long)i, w[i]); nz++; }
          LOG("B3-PEEKNZ frame=%llu addr=%#llx len=%llu nonzero=%llu%s", (unsigned long long)n, (unsigned long long)a, (unsigned long long)len, (unsigned long long)nz, first); } }
    { const char *pk = getenv("B3_PEEK");   // B3_PEEK=地址:字节数[,…]：在 B3_CAPTURE_FRAMES 的帧记下这段客体内存（十六进制 u32 与 float）| B3_PEEK=addr:bytes[,…]: on B3_CAPTURE_FRAMES frames log this guest memory (hex u32 and float)
      for (const char *q = pk; q && *q && capture_wanted(n); ) { char *e; uint64_t a = strtoull(q, &e, 0); unsigned len = *e == ':' ? (unsigned)strtoul(e + 1, &e, 0) : 16; if (len > 256) len = 256;
          char ln[3000]; size_t o = 0; const uint32_t *w = (const uint32_t *)(uintptr_t)a;
          for (unsigned i = 0; a && i < len / 4; i++) { float f; memcpy(&f, &w[i], 4); o += snprintf(ln + o, sizeof ln - o, " %08x(%g)", w[i], f); }
          LOG("B3-PEEK frame=%llu addr=%#llx%s", (unsigned long long)n, (unsigned long long)a, ln); q = *e == ',' ? e + 1 : e; if (e == q) break; } }
    const char *dir = getenv("B3_CAPTURE_DIR"); if (!dir || !*dir || !capture_wanted(n)) return;
    { const char *ss = getenv("B3_CAPTURE_SER");   // 同一帧顺带读回指定序号的贴图（nvn_mtl.m 的 b3_dump_tex_serial）| also read back the textures with these serials on the same frame (b3_dump_tex_serial in nvn_mtl.m)
      for (const char *q = ss; q && *q; ) { char *e; unsigned long long ser = strtoull(q, &e, 10); if (e == q) break;
          int b3_dump_tex_serial(uint64_t ser, const char *path); char pth[1200]; snprintf(pth, sizeof pth, "%s/tex_%llu_f%llu", dir, ser, (unsigned long long)n);
          int r = b3_dump_tex_serial(ser, pth); if (r) LOG("B3-TEXDUMP ser=%llu 失败 %d | failed", ser, r); q = *e == ',' ? e + 1 : e; } }
    { const char *wf = getenv("B3_CAPTURE_WHF");   // 宽x高:NVN格式[,…]：按尺寸与格式读回贴图（序号跨运行不稳定时用）| WxH:NVNformat[,…]: read back textures by size and format (when serials are unstable across runs)
      for (const char *q = wf; q && *q; ) { char *e; unsigned long w = strtoul(q, &e, 10), h = *e == 'x' ? strtoul(e + 1, &e, 10) : 0, fm = *e == ':' ? strtoul(e + 1, &e, 0) : 0;
          int b3_dump_tex_match(uint32_t w, uint32_t h, uint32_t fmt, const char *prefix); char pth[1200]; snprintf(pth, sizeof pth, "%s/whf_%lux%lu_%lx_f%llu", dir, w, h, fm, (unsigned long long)n);
          if (w && h) b3_dump_tex_match((uint32_t)w, (uint32_t)h, (uint32_t)fm, pth); q = *e == ',' ? e + 1 : e; if (e == q && *q) break; } }
    { const char *ts = getenv("B3_CAPTURE_TID");   // 按描述符号（跨运行稳定）| by descriptor id (stable across runs)
      for (const char *q = ts; q && *q; ) { char *e; unsigned long long id = strtoull(q, &e, 10); if (e == q) break;
          int b3_dump_tex_serial(uint64_t ser, const char *path); char pth[1200]; snprintf(pth, sizeof pth, "%s/tid_%llu_f%llu", dir, id, (unsigned long long)n);
          int r = b3_dump_tex_serial(id | (1ull << 63), pth); if (r) LOG("B3-TEXDUMP tid=%llu 失败 %d | failed", id, r); q = *e == ',' ? e + 1 : e; } }
    { static uint64_t lp, ld; uint64_t ps, dr; mtl_stats(&ps, &dr);   // 回放层累计通道 / 绘制数，记与上次截帧之差 | replay-layer cumulative passes / draws, logged as the delta since the previous capture
      extern uint64_t g_b3_skip[4];   // nvn_mtl.m（DM_B3）：draw 放弃原因累计 | nvn_mtl.m (DM_B3): cumulative draw bail-out reasons
      LOG("B3-FRAME-STATS n=%llu passes=%llu draws=%llu (since last capture) skip_total no_prog=%llu no_pass=%llu no_pso=%llu", (unsigned long long)n, (unsigned long long)(ps - lp), (unsigned long long)(dr - ld),
          (unsigned long long)g_b3_skip[0], (unsigned long long)g_b3_skip[1], (unsigned long long)g_b3_skip[2]); lp = ps; ld = dr; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (bpb != 4 || bw != 1) { LOG("B3-FRAME n=%llu 格式 %#x 不是 4 字节像素，不截 | not a 4-byte pixel format, skipped", (unsigned long long)n, t->d.format); return; }
    uint32_t w = crop && crop[2] > 0 ? (uint32_t)crop[2] : t->d.width, h = crop && crop[3] > 0 ? (uint32_t)crop[3] : t->d.height;
    if (w > t->d.width) w = t->d.width; if (h > t->d.height) h = t->d.height;
    uint8_t *px = malloc((size_t)w * h * 4); NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, 1};
    mtl_read_texels(t, 0, 0, &r, px, (size_t)w * 4, (size_t)w * h * 4);
    int bgra = t->d.format != NVN_FORMAT_RGBA8 && t->d.format != NVN_FORMAT_RGBA8_SRGB;   // 其他 4 字节格式按 BGRA 读（HYPOTHESIS，仅影响统计里的通道顺序）| other 4-byte formats are read as BGRA (HYPOTHESIS, only affects channel order in the stats)
    uint64_t sum[3] = {0}, black = 0; static uint32_t hist[4096]; memset(hist, 0, sizeof hist);
    char path[1200]; snprintf(path, sizeof path, "%s/frame_%04llu.ppm", dir, (unsigned long long)n);
    FILE *fp = fopen(path, "wb"); if (fp) fprintf(fp, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t c0 = px[4 * i + (bgra ? 2 : 0)], c1 = px[4 * i + 1], c2 = px[4 * i + (bgra ? 0 : 2)];
        sum[0] += c0; sum[1] += c1; sum[2] += c2; if (c0 < 8 && c1 < 8 && c2 < 8) black++;
        hist[(c0 >> 4) << 8 | (c1 >> 4) << 4 | c2 >> 4]++;
        if (fp) { uint8_t rgb[3] = {c0, c1, c2}; fwrite(rgb, 1, 3, fp); }
    }
    if (fp) fclose(fp);
    unsigned best = 0; for (unsigned k = 1; k < 4096; k++) if (hist[k] > hist[best]) best = k;
    double np = (double)w * h;
    LOG("B3-FRAME n=%llu size=%ux%u format=%#x black=%.4f mean=%.1f,%.1f,%.1f dominant=#%x%x%x(%.3f) file=%s", (unsigned long long)n, w, h, t->d.format, black / np,
        sum[0] / np, sum[1] / np, sum[2] / np, best >> 8, (best >> 4) & 15, best & 15, hist[best] / np, fp ? path : "(write failed)");
    free(px);
}

// ---------- 顶点状态查询（通用层的 va_st / vs_st 布局）| Vertex state queries (the base runtime's va_st / vs_st layout) ----------
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { const va_st *o = (const va_st *)s; *f = (NVNformat)o->fmt; *off = o->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return (int)((const va_st *)s)->stream; }
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return ((const vs_st *)s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return (int)((const vs_st *)s)->divisor; }

// ---------- 组合贴图句柄：低 20 位贴图号、其上采样器号（与通用层分离句柄的位布局一致）----------
// 绑定组合句柄 = 同一槽号上绑分离贴图 + 分离采样器（HYPOTHESIS：着色器翻译把组合采样槽 i 对到贴图槽 i 与采样器槽 i，核实）
// ---------- Combined texture handles: texture id in the low 20 bits, sampler id above (the same bit layout as the base runtime's separate handles) ----------
// Binding a combined handle = a separate texture plus a separate sampler on the same slot (HYPOTHESIS: shader translation maps combined slot i to texture slot i and sampler slot i, to verify in )
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tex) { (void)d; return (NVNtextureHandle)(uint64_t)(uint32_t)tex; }
static void bind_combined(NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) {
    static NVNT_nvnCommandBufferBindSeparateTextures ft; static NVNT_nvnCommandBufferBindSeparateSamplers fs;
    if (!ft) { ft = BASE(nvnCommandBufferBindSeparateTextures); fs = BASE(nvnCommandBufferBindSeparateSamplers); }
    NVNseparateTextureHandle t[32]; NVNseparateSamplerHandle sm[32];
    for (int off = 0; off < n; off += 32) {
        int k = n - off < 32 ? n - off : 32;
        for (int i = 0; i < k; i++) { uint64_t v = (uint64_t)h[off + i]; t[i].value = v; sm[i].value = (v >> 20) << 20; }   // 贴图槽存整个组合句柄：回放层按 &0xfffff 取贴图号，samp_tex 绑定按 >>20 取采样器号 | the texture slot keeps the whole combined handle: the replay layer takes the texture id with &0xfffff and samp_tex bindings take the sampler id with >>20
        ft(c, st, first + off, k, t); fs(c, st, first + off, k, sm);
    }
}
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_combined(c, st, first, n, h); }
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) { bind_combined(c, st, i, 1, &h); }

// ---------- 绘制变体 | Draw variants ----------
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) {
    static NVNT_nvnCommandBufferDrawElementsInstanced f; if (!f) f = BASE(nvnCommandBufferDrawElementsInstanced); f(c, pr, it, n, ib, 0, 0, 1);
}
// 间接绘制：只记参数地址，回放时由 Metal 在 GPU 上读（布局与 GL / Metal 相同：{count, instanceCount, firstIndex, baseVertex, baseInstance} / {count, instanceCount, first, baseInstance}）。
// 原先在录制时读内存，游戏用计算着色器在同一帧写这些参数，录制时还是旧值 / 0，人物的颜色绘制全被跳过，只剩深度预通道的黑影。
// Indirect draws: only the argument address is recorded and Metal reads it on the GPU at replay (layout as in GL / Metal: {count, instanceCount, firstIndex, baseVertex, baseInstance} / {count, instanceCount, first, baseInstance}).
// this used to read memory at record time, but the game writes these arguments with compute shaders in the same frame, so record time saw stale values / 0 and every character colour draw was skipped, leaving the black shapes of the depth prepass.
void b3_rec_draw_indirect(NVNcommandBuffer *c, int pr, int it, uint64_t ib, uint64_t ind);   // hle_nvn.c（DM_B3）
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    if (it == NVN_INDEX_TYPE_UNSIGNED_BYTE) { static int once; if (!once++) LOG("【红】B3 8 位索引的间接绘制未实现，跳过 | indirect draws with 8-bit indices are not implemented, skipped"); return; }
    b3_rec_draw_indirect(c, (int)pr, (int)it, ib, ind);
}
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind) { b3_rec_draw_indirect(c, (int)pr, 0, 0, ind); }
// GPU 驱动的批量绘制与间接派发：通用层当空操作，B3 的角色 / 怪物颜色绘制全在这里（每帧约 35 次），不画就只剩遮挡预通道的深度 → 黑影
// GPU-driven multi draws and indirect dispatch : no-ops in the base runtime; B3's character / monster colour draws all go through here (about 35 per frame), and without them only the occlusion prepass depth remains -> black silhouettes
void b3_rec_draw_indirect_count(NVNcommandBuffer *c, int pr, int it, uint64_t ib, uint64_t ind, uint64_t cnt, int max, int stride);   // hle_nvn.c（DM_B3）
void b3_rec_dispatch_indirect(NVNcommandBuffer *c, uint64_t ind, uint64_t caller);
static void mdic_note(const char *what, int max, int stride) {
    static int n; if (n++ < 4) LOG("B3-MDIC %s max=%d stride=%d", what, max, stride);
}
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    mdic_note("elements", max, (int)stride);
    if (max > 0) b3_rec_draw_indirect_count(c, (int)pr, (int)it, ib, ind, cnt, max, (int)(stride ? stride : 20));
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    mdic_note("arrays", max, (int)stride);
    if (max > 0) b3_rec_draw_indirect_count(c, (int)pr, 0, 0, ind, cnt, max, (int)(stride ? stride : 16));
}
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) { b3_rec_dispatch_indirect(c, ind, (uint64_t)__builtin_return_address(0)); }

// ---------- 提示类与查询 | Hints and queries ----------
// alpha 参考值：录制时记成全局值，回放层特化 alpha 测试时读（HYPOTHESIS：游戏用到的参考值只有 0、1/255、0.0001 几个，按录制顺序近似）
// Alpha reference: kept as a global at record time and read when the replay layer specializes the alpha test (HYPOTHESIS: the game only uses a few values such as 0, 1/255, 0.0001, so record order is a close enough approximation)
float g_b3_alpha_ref;
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float ref) { (void)c; g_b3_alpha_ref = ref; }   // alpha 测试未接入回放层（见 SetAlphaTest）| alpha test is not wired into the replay layer (see SetAlphaTest)
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t on) {
    // 深度钳制：超出远 / 近平面的片元钳到边界而不是裁掉。NVN 的 D24 深度在真机上本就把 > 1 的值钳到 1，Apple 上是 32 位浮点，不钳就裁掉——天空等贴着远平面的物体整片不画，旧画面留下来成拖影
    // Depth clamp: fragments beyond the far / near plane are clamped instead of clipped. On hardware NVN's D24 depth already clamps values above 1 to 1; on Apple it is 32-bit float and unclamped means clipped -- the sky and other far-plane geometry are not drawn at all and the old picture stays as ghosting 
    void b3_rec_depth_clamp(NVNcommandBuffer *c, int on); b3_rec_depth_clamp(c, on != 0);
}
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }      // 宿主没有 ZCull | the host has no ZCull
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *r) { const win_t *o = (const win_t *)w; *r = (NVNrectangle){o->crop[0], o->crop[1], o->crop[2], o->crop[3]}; }
// 命令缓冲里的围栏：执行到这里才算围上。录制时把同步对象指向“下一次提交”的栅栏值，等待它就是等这批命令所在的提交完成（HYPOTHESIS：这批命令在下一次提交里）
// Fence inside a command buffer: it counts only when execution reaches it. At record time the sync object is pointed at the fence value of the next submit, so waiting on it waits for the submit carrying these commands (HYPOTHESIS: these commands go in the next submit)
uint64_t mtl_last_fence(void);
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    (void)c; (void)cond; (void)flags; sync_t *o = (sync_t *)s; o->fenced = 1; o->fence = mtl_last_fence() + 1;
}

// Metal 层查不到某个着色器的翻译时调用（nvn_mtl.m，DM_B3）：严格模式记 B3-STOP 后中止，宽松模式每个键记一次 B3-GAP 后跳过
// Called by the Metal layer when a shader has no translation (nvn_mtl.m, DM_B3): strict mode logs B3-STOP and aborts; lenient mode logs B3-GAP once per key and skips it
void b3_shader_missing(const char *key, const char *why) {
    const char *e = getenv("DM_B3_LENIENT"); int lenient = e && !strcmp(e, "1"); extern uint64_t b3_svc_calls;
    enum { SEEN = 4096 }; static char seen[SEEN][17]; static int n;
    for (int i = 0; i < n; i++) if (!strcmp(seen[i], key)) return;
    if (n < SEEN) strlcpy(seen[n++], key, 17);
    LOG("%s kind=shader key=%s why=%s svc_calls=%llu", lenient ? "B3-GAP" : "B3-STOP", key, why, b3_svc_calls);
    if (!lenient) abort();
}

// ---------- 采样器查询 ----------
// ---------- Sampler queries ----------
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) { const samp_builder_t *o = (const samp_builder_t *)b; *lo = o->p.lod_min; *hi = o->p.lod_max; }

// ---------- 缓冲：池 + 偏移 + 大小；地址 = 池存储 + 偏移 ----------
// ---------- Buffers: pool + offset + size; address = pool storage + offset ----------
typedef struct { NVNdevice *dev; const pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; const pool_t *pool; int64_t off; uint64_t size; } buf_t;           FITS(buf_t, NVNbuffer);

NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = ((buf_builder_t *)b)->dev; memset(b, 0, sizeof(buf_builder_t)); ((buf_builder_t *)b)->dev = d; }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { ((buf_builder_t *)b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) { buf_builder_t *o = (buf_builder_t *)b; o->pool = (const pool_t *)p; o->off = off; o->size = n; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return ((const buf_builder_t *)b)->off; }
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return ((const buf_builder_t *)b)->size; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *s) {
    const buf_builder_t *i = (const buf_builder_t *)s; buf_t *o = (buf_t *)b;
    if (!i->pool || !i->pool->mem || i->off < 0 || (uint64_t)i->off + i->size > i->pool->size) {
        LOG("【红】B3 nvnBufferInitialize 存储不在池内 pool=%p off=%#llx size=%#llx | storage outside the pool", (const void *)i->pool, (long long)i->off, i->size);
        return 0;
    }
    *o = (buf_t){MAGIC('B', 'U', 'F', 'F'), i->pool, i->off, i->size};
    return 1;
}
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *b, const char *s) { (void)b; (void)s; }
NVN(nvnBufferFinalize, void, NVNbuffer *b) { ((buf_t *)b)->magic = 0; }
NVN(nvnBufferMap, void *, const NVNbuffer *b) { const buf_t *o = (const buf_t *)b; return o->pool->mem + o->off; }
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) { const buf_t *o = (const buf_t *)b; return (NVNbufferAddress)(o->pool->mem + o->off); }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }
NVN_NOPROTO(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b) { return (NVNmemoryPool *)((const buf_t *)b)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b) { return ((const buf_t *)b)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b) { return ((const buf_t *)b)->size; }
