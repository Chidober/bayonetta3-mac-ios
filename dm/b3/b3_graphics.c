// B3 的图形服务层（nvdrv、vi）：命令号与参数按 switchbrew「NV services」「Display services」页。
// 路线与通用层相同：NVN 本身由通用层的 nvnBootstrapLoader 接管层（dm/gfx/nvn）换成宿主实现，GPU 不在 ioctl 层模拟；这里只给 sdk 的图形初始化和 vi 提供文档写明的最小行为。
// nvdrv 的命令把 NvError（u32，0 = 成功）放在输出里；ioctl 先记录设备、命令值和缓冲大小，再按文档逐个实现，表外的按严格 / 宽松规则停止。
// B3 graphics service layer (nvdrv, vi): command ids and arguments follow the switchbrew pages "NV services" and "Display services".
// Same route as the base runtime: NVN itself is replaced by a host implementation through the base runtime's nvnBootstrapLoader takeover layer (dm/gfx/nvn); the GPU is not emulated at the ioctl level, and this file only gives the sdk's graphics init and vi the minimal documented behaviour.
// nvdrv commands put an NvError (u32, 0 = success) in their output; ioctls are logged first (device, command value, buffer sizes) and implemented one by one from the docs; anything not in the table stops under the strict / lenient rules.
#include "dm.h"
#include "b3_ipc.h"

static uint32_t nv_ok(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }   // 输出 NvError 0 | outputs NvError 0
static uint32_t ok_no_output(b3_call_t *c) { (void)c; return 0; }

#define RETURN_OBJECT(fn, name) static uint32_t fn(b3_call_t *c) { strlcpy(c->obj, name, sizeof c->obj); return 0; }
RETURN_OBJECT(vi_display_service, "vi.IApplicationDisplayService")   // vi:m 2 / vi:s 1 / vi:u 0 GetDisplayService
RETURN_OBJECT(vi_relay_service, "vi.IHOSBinderDriver")                // IApplicationDisplayService 100 GetRelayService
RETURN_OBJECT(vi_system_display_service, "vi.ISystemDisplayService")  // 101 GetSystemDisplayService
RETURN_OBJECT(vi_manager_display_service, "vi.IManagerDisplayService")   // 102 GetManagerDisplayService
// 显示与图层号：只有一个显示、一个应用图层，取固定值（DECISION）：显示 1，图层 1，录像图层 2 | Display and layer ids: one display and one application layer, fixed values (DECISION): display 1, layer 1, recording layer 2
static uint32_t vi_open_display(b3_call_t *c) { c->out[0] = 1; c->out[1] = 0; c->n_out = 2; return 0; }   // 1010 OpenDisplay(DisplayName) → u64
// 2020 OpenLayer(DisplayName, u64 图层, u64 aruid) → 输出缓冲里写原生窗口 parcel，u64 parcel 大小。parcel 头 {payload_size, payload_off, objects_size, objects_off}（libnx display/parcel.c），
// 载荷第 3 个字是 IGraphicBufferProducer 的 binder 号（libnx services/vi.c 读 payload[2]）；binder 号取 1（DECISION）。
// 2020 OpenLayer(DisplayName, u64 layer, u64 aruid) → writes the native window parcel into the output buffer, u64 parcel size. Parcel header {payload_size, payload_off, objects_size, objects_off} (libnx display/parcel.c);
// the 3rd payload word is the IGraphicBufferProducer binder id (libnx services/vi.c reads payload[2]); binder id 1 (DECISION), service name "dispdrv" (switchbrew Display services).
static uint32_t vi_open_layer(b3_call_t *c) {
    // 载荷（switchbrew「Display services」ParcelData）：0x0 = 2，0x4 进程号，0x8 binder 号，0xC 12 字节 0，0x18 "dispdrv"，0x20 8 字节 0；对象区一个 u32 0
    // Payload (switchbrew "Display services" ParcelData): 0x0 = 2, 0x4 process id, 0x8 binder id, 0xC 12 zero bytes, 0x18 "dispdrv", 0x20 8 zero bytes; the objects area is one u32 0
    uint32_t parcel[15] = {0x28, 0x10, 4, 0x38, 2, 0, 1, 0, 0, 0};
    memcpy(&parcel[4 + 6], "dispdrv", 8);
    b3_buf_t b = b3_out_buffer(c, 0);
    if (b.size < sizeof parcel) return 0x272;   // vi 的 InvalidArgument 一类（模块 114，描述 1）| vi InvalidArgument-like (module 114, description 1)
    memcpy((void *)(uintptr_t)b.addr, parcel, sizeof parcel);
    c->out[0] = sizeof parcel; c->out[1] = 0; c->n_out = 2; return 0;
}
// IHOSBinderDriver 2 GetNativeHandle(s32 binder 号, u32 类型) → 复制句柄：binder 的事件（libnx 取类型 0xF 当缓冲可用事件），新建一个未触发事件
// IHOSBinderDriver 2 GetNativeHandle(s32 binder id, u32 type) → copy handle: the binder's event (libnx uses type 0xF as the buffer-available event); a new unsignaled event
int svc_thread(int num, uint64_t *regs);   // hle_thread.c
static uint32_t binder_native_handle(b3_call_t *c) {
    uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 vi：建事件失败");
    c->copy[0] = (uint32_t)r[2]; c->n_copy = 1; return 0;
}
static uint32_t sc_separable_layer(b3_call_t *c) { c->out[0] = 1; c->out[1] = 0; c->out[2] = 2; c->out[3] = 0; c->n_out = 4; return 0; }   // ISelfController 44 → u64 图层, u64 录像图层 | u64 layer, u64 recording layer

// nvdrv 0 Open(0x5 输入缓冲：设备路径) → u32 fd, u32 NvError：记下路径，fd 从 1 递增；之后对该 fd 的 ioctl 按表外规则停止，日志里有设备名
// nvdrv 0 Open(type-0x5 input buffer: device path) → u32 fd, u32 NvError: record the path, fds count up from 1; later ioctls on that fd stop under the not-in-table rule, with the device name in the log 
#define NV_MAX_FD 32
static char g_nv_path[NV_MAX_FD][64];
static uint32_t nv_open(b3_call_t *c) {
    b3_buf_t b = b3_in_buffer(c, 0); char path[64] = {0};
    if (b.size) memcpy(path, (const void *)(uintptr_t)b.addr, b.size < sizeof path - 1 ? b.size : sizeof path - 1);
    int fd = 1; while (fd < NV_MAX_FD && g_nv_path[fd][0]) fd++;
    if (fd == NV_MAX_FD) die("B3 nvdrv：fd 用尽 | out of fds");
    strlcpy(g_nv_path[fd], path[0] ? path : "?", sizeof g_nv_path[fd]);
    LOG("B3-NV open path=%s fd=%d", g_nv_path[fd], fd);
    c->out[0] = (uint32_t)fd; c->out[1] = 0; c->n_out = 2; return 0;
}
// 4 QueryEvent(u32 fd, u32 EvtId) → NvError + 事件句柄：b3_nvhost.c 的同步点等待从不挂起，给一个新建的未触发事件（DECISION）
// 4 QueryEvent(u32 fd, u32 EvtId) → NvError + event handle: syncpoint waits in b3_nvhost.c never pend, so return a new unsignaled event (DECISION)
static uint32_t nv_query_event(b3_call_t *c) {
    uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 nvdrv：建事件失败 | event creation failed");
    LOG("B3-NV query_event fd=%u evt=%#x", c->n_in > 0 ? c->in[0] : 0, c->n_in > 1 ? c->in[1] : 0);
    c->copy[0] = (uint32_t)r[2]; c->n_copy = 1; c->out[0] = 0; c->n_out = 1; return 0;
}
static uint32_t nv_close(b3_call_t *c) {   // 2 Close(u32 fd) → NvError
    uint32_t fd = c->n_in ? c->in[0] : 0; if (fd && fd < NV_MAX_FD) { LOG("B3-NV close fd=%u path=%s", fd, g_nv_path[fd]); g_nv_path[fd][0] = 0; }
    c->out[0] = 0; c->n_out = 1; return 0;
}
// 1 Ioctl(u32 fd, u32 iocode, 0x21 入, 0x22 出)：先只记下设备与命令码后停止（严格）/ 回 NvError NotImplemented（宽松，0x1）——等看到实际用到哪些再按文档逐个实现
// 1 Ioctl(u32 fd, u32 iocode, type-0x21 in, type-0x22 out): for now log device and iocode, then stop (strict) / return NvError NotImplemented (lenient, 0x1) until the ones actually used are seen and implemented from the docs
int b3_nvhost_ioctl(int fd, const char *path, uint32_t code, uint8_t *buf, uint32_t sz, uint32_t *err);   // b3_nvhost.c
static uint32_t nv_ioctl(b3_call_t *c) {
    uint32_t fd = c->n_in > 0 ? c->in[0] : 0, code = c->n_in > 1 ? c->in[1] : 0;
    if (fd && fd < NV_MAX_FD && g_nv_path[fd][0]) {   // 先拷入输入，再交给 b3_nvhost.c；处理了就把结果写回输出缓冲 | copy the input in, hand it to b3_nvhost.c; if handled, write the result back to the output buffer
        b3_buf_t bi = b3_in_buffer(c, 0), bo = b3_out_buffer(c, 0);
        uint32_t sz = (uint32_t)(bo.size > bi.size ? bo.size : bi.size); if (sz > 0x10000) sz = 0x10000;
        uint8_t *tmp = calloc(1, sz ? sz : 1); if (!tmp) die("B3 nvdrv：内存不足 | out of memory");
        if (bi.size) memcpy(tmp, (const void *)(uintptr_t)bi.addr, bi.size < sz ? bi.size : sz);
        uint32_t err;
        if (b3_nvhost_ioctl((int)fd, g_nv_path[fd], code, tmp, sz, &err)) {
            if (bo.size) memcpy((void *)(uintptr_t)bo.addr, tmp, bo.size < sz ? bo.size : sz);
            if (err) LOG("B3-NV ioctl path=%s code=%#x NvError=%u", g_nv_path[fd], code, err);
            free(tmp); c->out[0] = err; c->n_out = 1; return 0;
        }
        free(tmp);
    }
    const char *e = getenv("DM_B3_LENIENT"); int lenient = e && !strcmp(e, "1"); extern uint64_t b3_svc_calls;
    LOG("%s kind=nv-ioctl path=%s fd=%u code=%#x svc_calls=%llu", lenient ? "B3-GAP" : "B3-STOP", fd < NV_MAX_FD ? g_nv_path[fd] : "?", fd, code, b3_svc_calls);
    if (!lenient) abort();
    c->out[0] = 1; c->n_out = 1; return 0;
}

// mm:u（nn::mmnv::IRequest，switchbrew「Display services」）：nvdec 解码前申请时钟。宿主没有时钟可调：记下请求的设定值，Get 原样回给它（DECISION）
// mm:u (nn::mmnv::IRequest, switchbrew "Display services"): clock requests made before nvdec decoding. The host has no clocks to set: remember the requested setting and Get returns it (DECISION)
#define MM_MAX_REQ 16
static uint32_t g_mm_n, g_mm_setting[MM_MAX_REQ + 1];
static uint32_t mm_initialize(b3_call_t *c) {   // 4 Initialize(Module, Priority, u32 EventClearMode) → u32 RequestId
    if (g_mm_n >= MM_MAX_REQ) die("B3 mm:u：请求用尽 | out of requests");
    c->out[0] = ++g_mm_n; c->n_out = 1; return 0;
}
static uint32_t mm_set_and_wait(b3_call_t *c) {   // 6 SetAndWait(u32 RequestId, Setting Min, Setting Max)
    uint32_t id = c->n_in > 0 ? c->in[0] : 0; if (id && id <= g_mm_n && c->n_in > 1) g_mm_setting[id] = c->in[1]; return 0;
}
static uint32_t mm_get(b3_call_t *c) {   // 7 Get(u32 RequestId) → Setting Current
    uint32_t id = c->n_in > 0 ? c->in[0] : 0; c->out[0] = id && id <= g_mm_n ? g_mm_setting[id] : 0; c->n_out = 1; return 0;
}

const b3_cmd_t b3_graphics_cmds[] = {
    {"vi.IApplicationDisplayService", B3_CMIF, 2020, vi_open_layer, "switchbrew VI: OpenLayer"},
    {"vi.IHOSBinderDriver", B3_CMIF, 1, ok_no_output, "switchbrew Nvnflinger: IHOSBinderDriver AdjustRefcount"},
    {"vi.IHOSBinderDriver", B3_CMIF, 2, binder_native_handle, "switchbrew Nvnflinger: IHOSBinderDriver GetNativeHandle"},
    {"vi.IApplicationDisplayService", B3_CMIF, 100, vi_relay_service, "switchbrew VI: GetRelayService"},
    {"vi.IApplicationDisplayService", B3_CMIF, 101, vi_system_display_service, "switchbrew VI: GetSystemDisplayService"},
    {"vi.IApplicationDisplayService", B3_CMIF, 102, vi_manager_display_service, "switchbrew VI: GetManagerDisplayService"},
    {"vi.IApplicationDisplayService", B3_CMIF, 1010, vi_open_display, "switchbrew VI: OpenDisplay"},
    {"am.ISelfController", B3_CMIF, 44, sc_separable_layer, "switchbrew AM: ISelfController CreateManagedDisplaySeparableLayer"},
    {"vi:m", B3_CMIF, 2, vi_display_service, "switchbrew VI: IManagerRootService GetDisplayService"},
    {"vi:s", B3_CMIF, 1, vi_display_service, "switchbrew VI: ISystemRootService GetDisplayService"},
    {"vi:u", B3_CMIF, 0, vi_display_service, "switchbrew VI: IApplicationRootService GetDisplayService"},
    {"mm:u", B3_CMIF, 4, mm_initialize, "switchbrew Display services: mm:u Initialize(Module, Priority, EventClearMode) → RequestId"},
    {"mm:u", B3_CMIF, 5, ok_no_output, "switchbrew Display services: mm:u Finalize(RequestId)"},
    {"mm:u", B3_CMIF, 6, mm_set_and_wait, "switchbrew Display services: mm:u SetAndWait(RequestId, Min, Max)"},
    {"mm:u", B3_CMIF, 7, mm_get, "switchbrew Display services: mm:u Get(RequestId) → Current"},
    {"nvdrv", B3_CMIF, 0, nv_open, "switchbrew NV: Open(path) → fd, NvError"},
    {"nvdrv", B3_CMIF, 1, nv_ioctl, "switchbrew NV: Ioctl(fd, iocode, in, out) → NvError（先记录后停止 | log then stop）"},
    {"nvdrv", B3_CMIF, 2, nv_close, "switchbrew NV: Close(fd) → NvError"},
    {"nvdrv", B3_CMIF, 4, nv_query_event, "switchbrew NV: QueryEvent(fd, EvtId) → NvError, event handle"},
    {"nvdrv", B3_CMIF, 3, nv_ok, "switchbrew NV: Initialize(u32 transfer memory size, process handle, transfer memory handle) → NvError"},
    {"nvdrv", B3_CMIF, 8, nv_ok, "switchbrew NV: SetAruid(u64) → NvError"},
    {"nvdrv", B3_CMIF, 13, ok_no_output, "switchbrew NV: SetGraphicsFirmwareMemoryMarginEnabled(bool)"},
};
const unsigned b3_graphics_ncmds = sizeof b3_graphics_cmds / sizeof *b3_graphics_cmds;
