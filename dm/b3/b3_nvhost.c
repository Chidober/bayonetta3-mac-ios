// B3 的 nvdrv 设备 ioctl：/dev/nvmap、/dev/nvhost-ctrl、通道设备（/dev/nvhost-nvdec 等）。布局按 switchbrew「NV services」。
// 用途：游戏在按键后用 nvdec 硬件解码 webm 影片（徽标、过场）；解码失败时游戏没有退路（宽松模式实测：ioctl 回错后 subsdk0 崩溃，VERIFIED）。
// 第一步（本文件）：句柄、同步点、命令缓冲映射与提交都按文档给出正确的簿记，提交立即完成（同步点直接加到目标值）；解码本身还没做，输出面保持原样（画面空白）。
// 表外的 ioctl 返回 0，由 b3_graphics.c 按严格 / 宽松规则停止。
// B3 nvdrv device ioctls: /dev/nvmap, /dev/nvhost-ctrl and channel devices (/dev/nvhost-nvdec etc.). Layouts follow switchbrew "NV services" .
// Purpose: after a key press the game decodes webm movies (logo, cutscenes) with nvdec; it has no fallback when decoding fails (lenient run: subsdk0 crashes after the ioctl errors, VERIFIED).
// First step (this file): handles, syncpoints, command-buffer mapping and submission keep correct documented bookkeeping, and submissions complete at once (syncpoints jump to their targets); decoding itself is not done yet and output surfaces stay as they are (blank picture).
// Ioctls not in the table return 0 and b3_graphics.c stops under the strict / lenient rules.
#include "dm.h"
#include <stdatomic.h>

#define NVMAP_MAX 4096
typedef struct { uint32_t size, align, flags, kind, used, iova; uint64_t addr; } nvmap_h_t;
static nvmap_h_t g_map[NVMAP_MAX];   // 句柄号 = 下标（0 不用）；id 与句柄号相同（DECISION）| handle = index (0 unused); id equals the handle (DECISION)
static uint32_t g_iova_next = 0x10000000;   // 通道地址（32 位）的分配游标；只增不减（DECISION）| channel address (32-bit) allocation cursor; grows only (DECISION)
#define NV_SYNCPT_MAX 192
static _Atomic uint32_t g_syncpt[NV_SYNCPT_MAX];
static uint64_t g_submits, g_submit_words;

static nvmap_h_t *map_get(uint32_t h) { return h && h < NVMAP_MAX && g_map[h].used ? &g_map[h] : NULL; }
uint64_t b3_nvmap_addr(uint32_t h) { nvmap_h_t *m = map_get(h); return m ? m->addr : 0; }
// 通道地址（MAP_CMD_BUFFER 发的 32 位 iova）→ 客体地址；查不到返回 0 | channel address (32-bit iova handed out by MAP_CMD_BUFFER) → guest address; 0 when not found
uint64_t b3_nv_iova_addr(uint64_t iova, uint64_t *avail) {
    for (uint32_t h = 1; h < NVMAP_MAX; h++) { nvmap_h_t *m = &g_map[h];
        if (m->used && m->iova && iova >= m->iova && iova < (uint64_t)m->iova + m->size) { if (avail) *avail = m->iova + m->size - iova; return m->addr + (iova - m->iova); } }
    return 0;
}

// 通道设备的同步点号：每个设备名固定一个（DECISION：nvdec 0x24、vic 0x25、其它 0x26；只要求不和别的冲突）| channel syncpoint ids: one fixed id per device name (DECISION: nvdec 0x24, vic 0x25, others 0x26; they only need to be distinct)
static uint32_t chan_syncpt(const char *path) { return strstr(path, "nvdec") ? 0x24 : strstr(path, "vic") ? 0x25 : 0x26; }

static int nvmap_ioctl(uint32_t code, uint8_t *b, uint32_t sz, uint32_t *err) {
    uint32_t *w = (uint32_t *)b;
    switch (code) {
    case 0xC0080101: {   // IOC_CREATE {u32 size in, u32 handle out}
        uint32_t h = 1; while (h < NVMAP_MAX && g_map[h].used) h++;
        if (h == NVMAP_MAX) die("B3 nvmap：句柄用尽 | out of handles");
        g_map[h] = (nvmap_h_t){.size = w[0], .used = 1}; w[1] = h; return 1; }
    case 0xC0080103: {   // IOC_FROM_ID {u32 id in, u32 handle out}
        if (!map_get(w[0])) { *err = 4; return 1; }   // NvError BadParameter（4）| NvError BadParameter (4)
        w[1] = w[0]; return 1; }
    case 0xC0200104: {   // IOC_ALLOC {handle, heapmask, flags, align, u8 kind, pad[7], u64 addr}
        nvmap_h_t *m = map_get(w[0]); if (!m) { *err = 4; return 1; }
        if (w[3] < 0x1000) w[3] = 0x1000;   // 对齐至少一页（与 Linux 驱动一致）| alignment is at least one page (as in the Linux driver)
        m->flags = w[2]; m->align = w[3]; m->kind = b[16]; memcpy(&m->addr, b + 24, 8); return 1; }
    case 0xC0180105: {   // IOC_FREE {handle in, pad, u64 address out, u32 size out, u32 flags out}
        nvmap_h_t *m = map_get(w[0]); if (!m) { *err = 4; return 1; }
        memcpy(b + 8, &m->addr, 8); w[4] = m->size; w[5] = m->flags; m->used = 0; return 1; }
    case 0xC00C0109: {   // IOC_PARAM {handle, param, result}：1 大小、2 对齐、3 基址（文档：报错）、4 堆（固定 0x40000000）、5 kind | 1 size, 2 alignment, 3 base (docs: error), 4 heap (always 0x40000000), 5 kind
        nvmap_h_t *m = map_get(w[0]); if (!m) { *err = 4; return 1; }
        switch (w[1]) { case 1: w[2] = m->size; break; case 2: w[2] = m->align; break; case 4: w[2] = 0x40000000; break; case 5: w[2] = m->kind; break;
                        default: *err = 4; break; }
        return 1; }
    case 0xC008010E:     // IOC_GET_ID {id out, handle in}
        if (!map_get(w[1])) { *err = 4; return 1; }
        w[0] = w[1]; return 1;
    }
    (void)sz; return 0;
}

static int ctrl_ioctl(uint32_t code, uint8_t *b, uint32_t sz, uint32_t *err) {
    uint32_t *w = (uint32_t *)b;
    switch (code) {
    case 0xC0080014: case 0xC008001A:   // SYNCPT_READ / SYNCPT_READ_MAX {id in, value out}：提交立即完成，当前值即最大值 | submissions complete at once, so the current value is the max
        if (w[0] >= NV_SYNCPT_MAX) { *err = 4; return 1; }
        w[1] = g_syncpt[w[0]]; return 1;
    case 0x40040015:                    // SYNCPT_INCR {id}
        if (w[0] >= NV_SYNCPT_MAX) { *err = 4; return 1; }
        g_syncpt[w[0]]++; return 1;
    case 0xC00C0016: case 0xC0100019: case 0xC010001D: case 0xC010001E: {   // SYNCPT_WAIT / WAITEX / WAIT_EVENT / WAIT_EVENT_EX {id, thresh, timeout, [value out]}
        if (w[0] >= NV_SYNCPT_MAX) { *err = 4; return 1; }
        uint32_t v = g_syncpt[w[0]];
        if ((int32_t)(v - w[1]) < 0) {   // 没有未完成的工作还没到阈值：说明簿记错了，停下查 | threshold not reached with no pending work: the bookkeeping is wrong, stop and look
            LOG("B3-NV syncpt wait 未达到 | not reached id=%u thresh=%u value=%u", w[0], w[1], v); die("B3 nvhost-ctrl：同步点等待不会完成 | syncpoint wait can never complete"); }
        if (code != 0xC00C0016) w[3] = v;
        return 1; }
    case 0xC004001C: return 1;          // SYNCPT_CLEAR_EVENT_WAIT {event_slot}：没有挂起的事件等待 | no pending event waits
    case 0xC004001F: case 0xC0040020:   // SYNCPT_ALLOC_EVENT / SYNCPT_FREE_EVENT {event_slot 0..0x3F}：等待从不挂起，槽位只做范围检查 | waits never pend; slots are only range-checked
        if (w[0] > 0x3F) { *err = 4; return 1; }
        return 1;
    case 0x40080021: return 1;          // SYNCPT_FREE_EVENT_BATCH {u64 mask}
    case 0xC0040022: w[0] = 0; return 1;   // SYNCPT_GET_SHIFT：0 = FIFO 关（文档两值之一；没有 GPU FIFO）| 0 = FIFO disabled (one of the two documented values; there is no GPU FIFO)
    }
    (void)sz; return 0;
}

void b3_host1x_cmdbuf(int fd, const uint32_t *w, uint32_t n);   // b3_host1x.c
static int chan_ioctl(int fd, const char *path, uint32_t code, uint8_t *b, uint32_t sz, uint32_t *err) {
    uint32_t *w = (uint32_t *)b, nr = code & 0xff, type = (code >> 8) & 0xff;
    if (code == 0x40044801 || code == 0x40040007) return 1;   // SetNvmapFd {fd} / SET_SUBMIT_TIMEOUT {timeout}：只有一个 nvmap，超时不适用 | one nvmap, no timeouts apply
    if (type != 0) return 0;
    switch (nr) {
    case 0x02: w[1] = chan_syncpt(path); return 1;   // GET_SYNCPOINT {module_id in, syncpt out}
    case 0x03: w[1] = 0; return 1;                    // GET_WAITBASE：文档写明总回 0 | the docs say it always returns 0
    case 0x09: case 0x0A: {   // MAP / UNMAP_CMD_BUFFER {num_handles, reserved, u8 is_compr, pad[3]} + {handle in, addr out}[]
        uint32_t n = w[0]; if (12 + 8ull * n > sz) { *err = 4; return 1; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t *e = (uint32_t *)(b + 12 + 8 * i); nvmap_h_t *m = map_get(e[0]);
            if (!m) { *err = 4; return 1; }
            if (nr == 0x09) {
                if (!m->iova) { m->iova = g_iova_next; g_iova_next += (m->size + 0xfffu) & ~0xfffu;
                                if (g_iova_next < m->iova) die("B3 nvhost：通道地址用尽 | out of channel addresses"); }
                e[1] = m->iova;
            }
        }
        return 1; }
    case 0x01: {   // SUBMIT {num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences} + cmdbufs[12] + relocs[16] + reloc_shifts[4] + syncpt_incrs[20] + fence_thresholds[4]（出）| (out)
        uint64_t nc = w[0], nr_ = w[1], ns = w[2], nf = w[3];
        uint64_t off_inc = 16 + 12 * nc + 16 * nr_ + 4 * nr_, off_fence = off_inc + 20 * ns;
        if (off_fence + 4 * nf > sz) { *err = 4; return 1; }
        for (uint64_t i = 0; i < nc; i++) {   // 命令缓冲交给 host1x 解析（VIC / NVDEC 执行在那里做）| hand command buffers to the host1x parser (VIC / NVDEC execution happens there)
            const uint32_t *e = (const uint32_t *)(b + 16 + 12 * i); nvmap_h_t *m = map_get(e[0]); g_submit_words += e[2];
            if (m && m->addr && (uint64_t)e[1] + 4ull * e[2] <= m->size) b3_host1x_cmdbuf(fd, (const uint32_t *)(uintptr_t)(m->addr + e[1]), e[2]);
        }
        {   // B3_NVDUMP=目录：每个设备前 60 次提交的命令缓冲按十六进制字写出（诊断，只写宿主目录）| B3_NVDUMP=dir: write each device's first 60 submissions' command buffers as hex words (diagnostic, host directory only)
            const char *dd = getenv("B3_NVDUMP"); static unsigned ndec, nvic, noth;
            unsigned *cnt = strstr(path, "nvdec") ? &ndec : strstr(path, "vic") ? &nvic : &noth;
            if (dd && *dd && *cnt < 60) {
                char fn[1200]; snprintf(fn, sizeof fn, "%s/%s_%03u.txt", dd, strrchr(path, '-') ? strrchr(path, '-') + 1 : "dev", (*cnt)++);
                FILE *f = fopen(fn, "w");
                for (uint64_t i = 0; f && i < nc; i++) {
                    const uint32_t *e = (const uint32_t *)(b + 16 + 12 * i); nvmap_h_t *m = map_get(e[0]);
                    fprintf(f, "cmdbuf %llu mem=%u off=%#x words=%u addr=%#llx iova=%#x\n", (unsigned long long)i, e[0], e[1], e[2], m ? (unsigned long long)m->addr : 0ull, m ? m->iova : 0);
                    if (m && m->addr && (uint64_t)e[1] + 4ull * e[2] <= m->size) { const uint32_t *w = (const uint32_t *)(uintptr_t)(m->addr + e[1]);
                        for (uint32_t k = 0; k < e[2]; k++) fprintf(f, "%08x%c", w[k], k % 8 == 7 ? '\n' : ' '); fputc('\n', f); }
                }
                // nvdec：方法 0x101（图像信息）指向的前 0x100 字节一并写出（值左移 8 位是通道地址）| nvdec: also write the first 0x100 bytes the method 0x101 (picture info) value points at (value << 8 is the channel address)
                for (uint64_t i = 0; f && (strstr(path, "nvdec") || strstr(path, "vic")) && i < nc; i++) {
                    const uint32_t *e = (const uint32_t *)(b + 16 + 12 * i); nvmap_h_t *m = map_get(e[0]);
                    if (!m || !m->addr || (uint64_t)e[1] + 4ull * e[2] > m->size) continue;
                    const uint32_t *w = (const uint32_t *)(uintptr_t)(m->addr + e[1]);
                    uint32_t want = strstr(path, "vic") ? 0x1c2 : 0x101;   // vic：0x1c2 配置结构 | vic: 0x1c2 config struct
                    for (uint32_t k = 0; k + 2 < e[2]; k++) if ((w[k] >> 28) == 1 && ((w[k] >> 16) & 0xfff) == 0x10 && (w[k + 1] == want || w[k + 1] == 0x1c8 || w[k + 1] == 0x1c9 || w[k + 1] == 0x100 || w[k + 1] == 0x101)) {
                        if (w[k + 1] != want) { uint64_t ga = b3_nv_iova_addr((uint64_t)w[k + 2] << 8, NULL); fprintf(f, "m%#x iova=%#llx guest=%#llx\n", w[k + 1], (unsigned long long)w[k + 2] << 8, (unsigned long long)ga); continue; }
                        uint64_t av = 0, ga = b3_nv_iova_addr((uint64_t)w[k + 2] << 8, &av);
                        if (ga) { const uint32_t *pi = (const uint32_t *)(uintptr_t)ga; fprintf(f, "picinfo iova=%#llx\n", (unsigned long long)w[k + 2] << 8);
                            for (unsigned q = 0; q < 64 && 4ull * q < av; q++) fprintf(f, "%08x%c", pi[q], q % 8 == 7 ? '\n' : ' '); fputc('\n', f); }
                    }
                }
                if (f) fclose(f);
                if (*cnt == 1) { snprintf(fn, sizeof fn, "%s/nvmap.txt", dd); FILE *g = fopen(fn, "w");   // 句柄表（之后用 iova 反查）| handle table (to resolve iovas later)
                    for (uint32_t h = 1; g && h < NVMAP_MAX; h++) if (g_map[h].used) fprintf(g, "h=%u addr=%#llx size=%#x iova=%#x\n", h, (unsigned long long)g_map[h].addr, g_map[h].size, g_map[h].iova);
                    if (g) fclose(g); }
            }
        }
        for (uint64_t i = 0; i < ns; i++) {
            uint32_t *e = (uint32_t *)(b + off_inc + 20 * i);
            if (e[0] >= NV_SYNCPT_MAX) { *err = 4; return 1; }
            uint32_t v = atomic_fetch_add(&g_syncpt[e[0]], e[1]) + e[1];   // 立即完成：同步点直接加到目标 | completes at once: the syncpoint jumps to its target
            if (i < nf) ((uint32_t *)(b + off_fence))[i] = v;
        }
        if (g_submits++ % 300 == 0)
            LOG("B3-NV submit path=%s n=%llu cmdbufs=%llu relocs=%llu incrs=%llu fences=%llu words_total=%llu（未解码 | not decoded）", path,
                (unsigned long long)g_submits, (unsigned long long)nc, (unsigned long long)nr_, (unsigned long long)ns, (unsigned long long)nf, (unsigned long long)g_submit_words);
        return 1; }
    }
    return 0;
}

// 返回 1 = 已处理（*err 是 NvError），0 = 表外。buf 是输入缓冲内容已复制好的输出区，大小 sz（来自 iocode 的大小域或缓冲大小）
// Returns 1 = handled (*err is the NvError), 0 = not in the table. buf is the output area with the input already copied in, of size sz (from the iocode size field or the buffer size)
int b3_nvhost_ioctl(int fd, const char *path, uint32_t code, uint8_t *buf, uint32_t sz, uint32_t *err) {
    *err = 0;
    uint32_t need = (code >> 16) & 0x3fff;
    if (sz < need) { LOG("B3-NV ioctl 缓冲太小 | buffer too small path=%s code=%#x sz=%u need=%u", path, code, sz, need); *err = 4; return 1; }
    if (!strcmp(path, "/dev/nvmap")) return nvmap_ioctl(code, buf, sz, err);
    if (!strcmp(path, "/dev/nvhost-ctrl")) return ctrl_ioctl(code, buf, sz, err);
    if (!strncmp(path, "/dev/nvhost-", 12)) return chan_ioctl(fd, path, code, buf, sz, err);
    return 0;
}
