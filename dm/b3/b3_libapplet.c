// B3 的库 applet 框架（ILibraryAppletCreator / ILibraryAppletAccessor / IStorage / IStorageAccessor），命令号与语义按 switchbrew「Applet Manager services」页。
// 每个库 applet 按 appletId 在 g_applets 里登记一个实现：Start 时读入数据（IStorage 队列）、写出数据、置“完成”并触发状态变化事件。没登记的 appletId 在 Start 时按严格 / 宽松规则停止。
// 存储对象名形如 "am.IStorage#3"，同号的 "am.IStorageAccessor#3" 读写同一块数据；applet 访问器名形如 "am.ILibraryAppletAccessor#1"。
// B3 library applet framework (ILibraryAppletCreator / ILibraryAppletAccessor / IStorage / IStorageAccessor); command ids and semantics follow the switchbrew page "Applet Manager services".
// Each library applet registers an implementation by appletId in g_applets: on Start it reads the input data (IStorage queue), writes output data, marks itself completed and signals the state-changed event. An unregistered appletId stops on Start under the strict / lenient rules.
// Storage objects are named like "am.IStorage#3", and the "am.IStorageAccessor#3" of the same number reads and writes the same data; applet accessors are named like "am.ILibraryAppletAccessor#1".
#include "dm.h"
#include "b3_ipc.h"

int svc_thread(int num, uint64_t *regs);   // hle_thread.c
#define B3_STORAGE_MAX 64
#define B3_APPLET_MAX 16
#define AM_NOT_AVAILABLE 0x680   // AM 模块 128 描述 3：没有可取的数据 | AM module 128 description 3: nothing to pop

typedef struct { int used; uint64_t size; uint8_t *data; } b3_storage_t;
typedef struct { int used, started, completed; uint32_t applet_id, mode, ev_read, ev_write; int in[8], n_in, out[8], n_out; } b3_applet_t;
static b3_storage_t g_storage[B3_STORAGE_MAX];
static b3_applet_t g_applet[B3_APPLET_MAX];

static int storage_new(uint64_t size) {
    for (int i = 1; i < B3_STORAGE_MAX; i++)
        if (!g_storage[i].used) { g_storage[i] = (b3_storage_t){1, size, calloc(1, size ? size : 1)}; return i; }
    die("B3 IStorage 表满 | B3 IStorage table full");
}
static int instance(const char *name) { const char *p = strchr(name, '#'); return p ? atoi(p + 1) : 0; }
static b3_storage_t *storage_of(const char *name) { int i = instance(name); return i > 0 && i < B3_STORAGE_MAX && g_storage[i].used ? &g_storage[i] : NULL; }
static b3_applet_t *applet_of(const char *name) { int i = instance(name); return i > 0 && i < B3_APPLET_MAX && g_applet[i].used ? &g_applet[i] : NULL; }
static void name_of(char out[40], const char *base, int i) { snprintf(out, 40, "%s#%d", base, i); }

// ---------- 各库 applet 的实现 | Library applet implementations ----------
// 手柄支持（0xC）：输入依次是 CommonArguments、ControllerSupportArgPrivate、ControllerSupportArg（libnx hid_la.c 的推入顺序）；输出 0xC 字节 ControllerSupportResultInfoInternal：
// s8 player_count、u32 selected_id（偏移 4）、u32 res（偏移 8）。宿主已在 hid 共享内存写了掌机手柄，所以回“成功、1 个玩家、Handheld(0x20)”。
// Controller support (0xC): inputs are CommonArguments, ControllerSupportArgPrivate, ControllerSupportArg in that order (libnx hid_la.c push order); output is the 0xC-byte ControllerSupportResultInfoInternal:
// s8 player_count, u32 selected_id (offset 4), u32 res (offset 8). The host has written a handheld controller into hid shared memory, so the answer is "success, 1 player, Handheld (0x20)".
static void applet_controller(b3_applet_t *a) {
    if (a->n_in >= 2) {
        const uint8_t *p = g_storage[a->in[1]].data;   // HidLaControllerSupportArgPrivate：u32 大小、u32 参数大小、u8 标志、u8 标志、u8 模式、u8 调用者、u32 样式、u32 握持方式 | arg private
        if (g_storage[a->in[1]].size >= 0x14) LOG("B3-APPLET controller mode=%u style_set=%#x hold_type=%u", p[10], *(const uint32_t *)(p + 12), *(const uint32_t *)(p + 16));
    }
    if (a->n_in >= 3 && g_storage[a->in[2]].size >= 4) {
        const int8_t *q = (const int8_t *)g_storage[a->in[2]].data;   // HidLaControllerSupportArgHeader：player_count_min、player_count_max、…、enable_single_mode | arg header
        LOG("B3-APPLET controller player_count_min=%d player_count_max=%d", q[0], q[1]);
    }
    int o = storage_new(0xc);
    uint8_t *r = g_storage[o].data; r[0] = 1; *(uint32_t *)(r + 4) = 0x20; *(uint32_t *)(r + 8) = 0;
    a->out[a->n_out++] = o;
}
typedef struct { uint32_t id; const char *name; void (*run)(b3_applet_t *); } b3_applet_impl_t;
static const b3_applet_impl_t g_applets[] = {
    {0x0c, "controller", applet_controller},   // switchbrew：LibraryAppletController
};

// ---------- ILibraryAppletCreator ----------
static uint32_t lac_create_applet(b3_call_t *c) {   // 0 CreateLibraryApplet(u32 appletId, u32 模式) | (u32 appletId, u32 mode)
    for (int i = 1; i < B3_APPLET_MAX; i++)
        if (!g_applet[i].used) {
            g_applet[i] = (b3_applet_t){.used = 1, .applet_id = c->n_in > 0 ? c->in[0] : 0, .mode = c->n_in > 1 ? c->in[1] : 0};
            g_applet[i].ev_read = 0; name_of(c->obj, "am.ILibraryAppletAccessor", i);
            LOG("B3-APPLET create id=%#x mode=%u accessor=%d", g_applet[i].applet_id, g_applet[i].mode, i);
            return 0;
        }
    die("B3 库 applet 表满 | B3 library applet table full");
}
static uint32_t lac_create_storage(b3_call_t *c) {   // 10 CreateStorage(s64 大小) | (s64 size)
    uint64_t size = c->n_in >= 2 ? c->in[0] | (uint64_t)c->in[1] << 32 : 0;
    name_of(c->obj, "am.IStorage", storage_new(size)); return 0;
}
// ---------- IStorage / IStorageAccessor ----------
static uint32_t st_open(b3_call_t *c) { if (!storage_of(c->self)) return AM_NOT_AVAILABLE; name_of(c->obj, "am.IStorageAccessor", instance(c->self)); return 0; }   // IStorage 0 Open
static uint32_t sa_get_size(b3_call_t *c) {   // IStorageAccessor 0 GetSize → s64
    b3_storage_t *s = storage_of(c->self); if (!s) return AM_NOT_AVAILABLE;
    c->out[0] = (uint32_t)s->size; c->out[1] = (uint32_t)(s->size >> 32); c->n_out = 2; return 0;
}
static uint32_t sa_write(b3_call_t *c) {   // IStorageAccessor 10 Write(s64 偏移, 输入缓冲) | (s64 offset, input buffer)
    b3_storage_t *s = storage_of(c->self); b3_buf_t b = b3_in_buffer(c, 0); uint64_t off = c->n_in >= 2 ? c->in[0] | (uint64_t)c->in[1] << 32 : 0;
    if (!s || off > s->size || b.size > s->size - off) return 0x1080;   // AM 模块 128 描述 8：越界（DECISION：取最近的 AM 错误码）| AM module 128 description 8: out of range (DECISION: closest AM error)
    memcpy(s->data + off, (const void *)(uintptr_t)b.addr, b.size); return 0;
}
static uint32_t sa_read(b3_call_t *c) {   // IStorageAccessor 11 Read(s64 偏移, 输出缓冲) | (s64 offset, output buffer)
    b3_storage_t *s = storage_of(c->self); b3_buf_t b = b3_out_buffer(c, 0); uint64_t off = c->n_in >= 2 ? c->in[0] | (uint64_t)c->in[1] << 32 : 0;
    if (!s || off > s->size) return 0x1080;
    uint64_t n = b.size < s->size - off ? b.size : s->size - off;
    memcpy((void *)(uintptr_t)b.addr, s->data + off, n); return 0;
}
// ---------- ILibraryAppletAccessor ----------
static uint32_t la_state_event(b3_call_t *c) {   // 0 GetAppletStateChangedEvent → 复制句柄 | copy handle
    b3_applet_t *a = applet_of(c->self); if (!a) return AM_NOT_AVAILABLE;
    if (!a->ev_read) { uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 applet：建事件失败"); a->ev_write = (uint32_t)r[1]; a->ev_read = (uint32_t)r[2]; if (a->completed) { uint64_t s[8] = {a->ev_write}; svc_thread(0x11, s); } }
    c->copy[0] = a->ev_read; c->n_copy = 1; return 0;
}
static uint32_t la_is_completed(b3_call_t *c) { b3_applet_t *a = applet_of(c->self); c->out[0] = a && a->completed; c->n_out = 1; return 0; }   // 1 IsCompleted → u8
static uint32_t la_push_in(b3_call_t *c) {   // 100 PushInData(IStorage)
    b3_applet_t *a = applet_of(c->self); if (!a || !c->n_in_obj) return AM_NOT_AVAILABLE;
    int s = instance(c->in_obj[0]); if (a->n_in < 8 && s > 0) a->in[a->n_in++] = s;
    LOG("B3-APPLET push id=%#x storage=%d size=%#llx", a->applet_id, s, s > 0 ? g_storage[s].size : 0); return 0;
}
static uint32_t la_start(b3_call_t *c) {   // 10 Start：同步执行登记的实现，完成后触发状态变化事件 | runs the registered implementation synchronously, then signals the state-changed event
    b3_applet_t *a = applet_of(c->self); if (!a) return AM_NOT_AVAILABLE;
    const b3_applet_impl_t *impl = NULL;
    for (unsigned i = 0; i < sizeof g_applets / sizeof *g_applets; i++) if (g_applets[i].id == a->applet_id) impl = &g_applets[i];
    if (!impl) {
        const char *e = getenv("DM_B3_LENIENT"); int lenient = e && !strcmp(e, "1"); extern uint64_t b3_svc_calls;
        LOG("%s kind=ipc service=am.applet.%#x proto=cmif cmd=10 svc_calls=%llu", lenient ? "B3-GAP" : "B3-STOP", a->applet_id, b3_svc_calls);
        if (!lenient) abort();
    } else impl->run(a);
    a->started = a->completed = 1;
    if (a->ev_read) { uint64_t r[8] = {a->ev_write}; svc_thread(0x11, r); }
    LOG("B3-APPLET done id=%#x name=%s out=%d", a->applet_id, impl ? impl->name : "unregistered", a->n_out);
    return 0;
}
static uint32_t la_get_result(b3_call_t *c) { b3_applet_t *a = applet_of(c->self); return a && a->completed ? 0 : AM_NOT_AVAILABLE; }   // 30 GetResult
static uint32_t la_pop_out(b3_call_t *c) {   // 101 PopOutData → IStorage
    b3_applet_t *a = applet_of(c->self); if (!a || !a->n_out) return AM_NOT_AVAILABLE;
    name_of(c->obj, "am.IStorage", a->out[0]); memmove(a->out, a->out + 1, --a->n_out * sizeof *a->out); return 0;
}
static uint32_t la_ok(b3_call_t *c) { (void)c; return 0; }

// IApplicationFunctions 1 PopLaunchParameter(u32 种类) → IStorage。种类 2 = 预选用户：0x88 字节 {u32 魔数 0xC79497CA, u32 1, Uid, 其余 0}（libnx applet.h 的 PreselectedUser 布局），
// 只能取一次（文档：取出后从系统状态里删除）；其他种类、或已取过 → 0x680 没有数据。
// IApplicationFunctions 1 PopLaunchParameter(u32 kind) → IStorage. Kind 2 = preselected user: 0x88 bytes {u32 magic 0xC79497CA, u32 1, Uid, rest 0} (the PreselectedUser layout in libnx applet.h),
// poppable once (docs: popping removes it from the system state); other kinds, or a second pop → 0x680 no data.
static uint32_t af_pop_launch_parameter(b3_call_t *c) {
    static int popped; uint32_t kind = c->n_in ? c->in[0] : 0;
    LOG("B3-APPLET launch_parameter kind=%u%s", kind, kind == 2 && !popped ? " → preselected user" : " → none");
    if (kind != 2 || popped) return AM_NOT_AVAILABLE;
    popped = 1; int o = storage_new(0x88); uint8_t *d = g_storage[o].data;
    uint32_t magic = 0xC79497CA, one = 1; uint64_t uid[2] = {B3_USER_UID_LO, B3_USER_UID_HI};
    memcpy(d, &magic, 4); memcpy(d + 4, &one, 4); memcpy(d + 8, uid, 16);
    name_of(c->obj, "am.IStorage", o); return 0;
}

const b3_cmd_t b3_libapplet_cmds[] = {
    {"am.IApplicationFunctions", B3_CMIF, 1, af_pop_launch_parameter, "switchbrew AM: IApplicationFunctions PopLaunchParameter"},
    {"am.ILibraryAppletCreator", B3_CMIF, 0, lac_create_applet, "switchbrew AM: CreateLibraryApplet"},
    {"am.ILibraryAppletCreator", B3_CMIF, 10, lac_create_storage, "switchbrew AM: CreateStorage"},
    {"am.IStorage", B3_CMIF, 0, st_open, "switchbrew AM: IStorage Open"},
    {"am.IStorageAccessor", B3_CMIF, 0, sa_get_size, "switchbrew AM: IStorageAccessor GetSize"},
    {"am.IStorageAccessor", B3_CMIF, 10, sa_write, "switchbrew AM: IStorageAccessor Write"},
    {"am.IStorageAccessor", B3_CMIF, 11, sa_read, "switchbrew AM: IStorageAccessor Read"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 0, la_state_event, "switchbrew AM: GetAppletStateChangedEvent"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 1, la_is_completed, "switchbrew AM: IsCompleted"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 10, la_start, "switchbrew AM: Start"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 20, la_ok, "switchbrew AM: RequestExit"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 25, la_ok, "switchbrew AM: Terminate"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 30, la_get_result, "switchbrew AM: GetResult"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 100, la_push_in, "switchbrew AM: PushInData"},
    {"am.ILibraryAppletAccessor", B3_CMIF, 101, la_pop_out, "switchbrew AM: PopOutData"},
};
const unsigned b3_libapplet_ncmds = sizeof b3_libapplet_cmds / sizeof *b3_libapplet_cmds;
