// B3 的 applet 层服务（appletOE 及其 IApplicationProxy 子接口）：命令号与参数、返回值的含义都按 switchbrew「Applet Manager services」页，行为取文档写明的最小语义。
// 事件句柄借 hle_thread.c 的内核事件（SVC 0x45 CreateEvent / 0x11 SignalEvent），客户端照常用 WaitSynchronization 等它。
// B3 applet-layer services (appletOE and its IApplicationProxy sub-interfaces): command ids and the meaning of arguments and return values follow the switchbrew page "Applet Manager services"; behaviour takes the minimal documented semantics.
// Event handles borrow hle_thread.c's kernel events (SVC 0x45 CreateEvent / 0x11 SignalEvent), and the client waits on them with WaitSynchronization as usual.
#include <pthread.h>
#include <time.h>
#include "dm.h"
#include "b3_ipc.h"

int svc_thread(int num, uint64_t *regs);   // hle_thread.c
uint32_t b3_shmem_new(uint64_t size, const char *tag);   // b3_kernel.c

// 新建一个内核事件，返回读端（交给客户端）；写端留在 *write_end | Create a kernel event and return the read end (for the client); the write end goes to *write_end
static uint32_t event_new(uint32_t *write_end) {
    uint64_t r[8] = {0};
    if (!svc_thread(0x45, r) || r[0]) die("B3 applet：建事件失败 | B3 applet: event creation failed");
    *write_end = (uint32_t)r[1]; return (uint32_t)r[2];
}

#define RETURN_OBJECT(fn, name) static uint32_t fn(b3_call_t *c) { strlcpy(c->obj, name, sizeof c->obj); return 0; }
RETURN_OBJECT(oe_open_application_proxy, "am.IApplicationProxy")   // appletOE 0 OpenApplicationProxy
RETURN_OBJECT(ap_common_state_getter, "am.ICommonStateGetter")     // IApplicationProxy 0
RETURN_OBJECT(ap_self_controller, "am.ISelfController")            // IApplicationProxy 1
RETURN_OBJECT(ap_window_controller, "am.IWindowController")        // IApplicationProxy 2
RETURN_OBJECT(ap_audio_controller, "am.IAudioController")          // IApplicationProxy 3
RETURN_OBJECT(ap_display_controller, "am.IDisplayController")      // IApplicationProxy 4
RETURN_OBJECT(ap_library_applet_creator, "am.ILibraryAppletCreator")   // IApplicationProxy 11
RETURN_OBJECT(ap_application_functions, "am.IApplicationFunctions")    // IApplicationProxy 20
RETURN_OBJECT(ap_debug_functions, "am.IDebugFunctions")            // IApplicationProxy 1000
RETURN_OBJECT(pctl_create_service, "pctl.IParentalControlService")  // pctl 0 CreateService / 1 CreateServiceWithoutInitialize
RETURN_OBJECT(time_timezone_service, "time.ITimeZoneService")      // time:u 3 GetTimeZoneService
RETURN_OBJECT(apm_open_session, "apm.ISession")                    // apm 0 OpenSession
RETURN_OBJECT(nifm_general_service, "nifm.IGeneralService")         // nifm:u 5 CreateGeneralService
RETURN_OBJECT(acc_baas_manager, "acc.IManagerForApplication")       // acc:u0 101 GetBaasAccountManagerForApplication
RETURN_OBJECT(nifm_create_request, "nifm.IRequest")                 // nifm.IGeneralService 4 CreateRequest

// ---------- 网络连接请求（nifm.IRequest，switchbrew「Network Interface services」）：宿主不提供网络，请求立即结束且不被接受 ----------
// 游戏在章节菜单光标停到 Online Ranking 时提交连接请求；不实现时崩溃。状态 1 = Free（已处理完、未接受），结果为「网络通信已禁用」（NIFM 模块 110，描述 1111），两个事件都触发，游戏按无网络处理（DECISION）
// ---------- Network connection requests (nifm.IRequest, switchbrew "Network Interface services"): the host provides no network, so requests finish at once and are not accepted  ----------
// The game submits a connection request when the chapter-menu cursor rests on Online Ranking; it crashed while this was unimplemented. State 1 = Free (finished, not accepted), result "network communication disabled" (NIFM module 110, description 1111), both events signaled, so the game treats it as offline (DECISION)
static uint32_t nifm_req_state(b3_call_t *c) { c->out[0] = 1; c->n_out = 1; return 0; }   // 0 GetRequestState → 1（Free）
static uint32_t nifm_req_result(b3_call_t *c) { (void)c; return 110 | (1111u << 9); }   // 1 GetResult → 网络通信已禁用 | network communication disabled
static uint32_t nifm_req_events(b3_call_t *c) {   // 2 GetSystemEventReadableHandles → 两个已触发的事件 | two signaled events
    for (int i = 0; i < 2; i++) { uint32_t w; c->copy[i] = event_new(&w); uint64_t r[8] = {w}; svc_thread(0x11, r); }
    c->n_copy = 2; return 0;
}
static uint32_t nifm_req_applet_info(b3_call_t *c) { c->out[0] = c->out[1] = c->out[2] = 0; c->n_out = 3; return 0; }   // 21 GetAppletInfo → 小程序 0、模式 0、大小 0：不弹网络设置小程序 | applet 0, mode 0, size 0: no network-settings applet

// ---------- 账号（acc:u0，switchbrew「Account services」IAccountServiceForApplication）：宿主只有一个用户 B3_USER_UID，已打开 ----------
// ---------- Accounts (acc:u0, switchbrew "Account services" IAccountServiceForApplication): the host has one user, B3_USER_UID, and it is open ----------
static int is_b3_user(const uint32_t *w) { uint64_t u[2]; memcpy(u, w, 16); return u[0] == B3_USER_UID_LO && u[1] == B3_USER_UID_HI; }
static void put_user_list(b3_call_t *c) {   // 输出缓冲：Uid 数组，第一项是宿主用户，其余 0 | Output buffer: Uid array, the host user first, the rest 0
    b3_buf_t b = b3_out_buffer(c, 0); if (!b.size) return;
    memset((void *)(uintptr_t)b.addr, 0, b.size);
    if (b.size >= 16) { uint64_t u[2] = {B3_USER_UID_LO, B3_USER_UID_HI}; memcpy((void *)(uintptr_t)b.addr, u, 16); }
}
static uint32_t acc_user_count(b3_call_t *c) { c->out[0] = 1; c->n_out = 1; return 0; }                       // 0 GetUserCount → s32
static uint32_t acc_user_exists(b3_call_t *c) { c->out[0] = c->n_in >= 4 && is_b3_user(c->in); c->n_out = 1; return 0; }   // 1 GetUserExistence(Uid) → bool
static uint32_t acc_list_users(b3_call_t *c) { put_user_list(c); return 0; }                                  // 2 ListAllUsers / 3 ListOpenUsers
static uint32_t acc_last_opened(b3_call_t *c) { uint64_t u[2] = {B3_USER_UID_LO, B3_USER_UID_HI}; memcpy(c->out, u, 16); c->n_out = 4; return 0; }   // 4 GetLastOpenedUser → Uid
static uint32_t acc_no_users(b3_call_t *c) { b3_buf_t b = b3_out_buffer(c, 0); if (b.size) memset((void *)(uintptr_t)b.addr, 0, b.size); return 0; }   // 131 ListOpenContextStoredUsers：没有存过的上下文 | no stored contexts
// pl:u 5 GetSharedFontInOrderOfPriority(LanguageCode) → u8 已载入, u32 个数：没有系统共享字体数据（不是游戏文件），报「已载入、0 个」；游戏用自带字体（DECISION，实测读进度存档后开始调用）
// pl:u 5 GetSharedFontInOrderOfPriority(LanguageCode) → u8 loaded, u32 count : there is no system shared font data (it is not a game file), so report "loaded, 0 fonts"; the game uses its own fonts (DECISION; measured to start being called after loading a progress save)
static uint32_t pl_font_order(b3_call_t *c) { c->out[0] = 1; c->out[1] = 0; c->n_out = 2; return 0; }
static uint32_t pl_load_state(b3_call_t *c) { c->out[0] = 1; c->n_out = 1; return 0; }   // pl:u 1 GetLoadState → 1（已载入）| → 1 (loaded)
static uint32_t pl_shared_memory(b3_call_t *c) { static uint32_t h; if (!h) h = b3_shmem_new(0x1100000, "pl"); c->copy[0] = h; c->n_copy = 1; return 0; }   // pl:u 4：共享字体内存（全 0，系统大小 0x1100000）| shared font memory (all zero, system size 0x1100000)
static uint32_t pl_zero(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }          // pl:u 2 / 3 GetSize / GetSharedMemoryAddressOffset → 0
static uint32_t aoc_count(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }   // aoc:u 2 CountAddOnContent → 0（本体 v0，无追加内容）| → 0 (base v0, no add-on content)
// time.ITimeZoneService 101 ToCalendarTimeWithMyRule(PosixTime s64) → CalendarTime（8 字节）+ CalendarAdditionalInfo（0x18 字节）；宿主本地时区
// time.ITimeZoneService 101 ToCalendarTimeWithMyRule(PosixTime s64) → CalendarTime (8 bytes) + CalendarAdditionalInfo (0x18 bytes); the host local time zone 
static uint32_t tz_to_calendar(b3_call_t *c) {
    if (c->n_in < 2) return 0x2c99;
    int64_t t; memcpy(&t, c->in, 8); time_t tt = (time_t)t; struct tm g; localtime_r(&tt, &g);   // 按宿主本地时区（存档时间按用户所在时区显示）| host local time zone (save timestamps in the user's time zone)
    uint8_t o[0x20] = {0}; uint16_t y = (uint16_t)(g.tm_year + 1900); memcpy(o, &y, 2);
    o[2] = (uint8_t)(g.tm_mon + 1); o[3] = (uint8_t)g.tm_mday; o[4] = (uint8_t)g.tm_hour; o[5] = (uint8_t)g.tm_min; o[6] = (uint8_t)g.tm_sec;
    uint32_t wd = (uint32_t)g.tm_wday, yd = (uint32_t)g.tm_yday; memcpy(o + 8, &wd, 4); memcpy(o + 12, &yd, 4); { char tz[8] = {0}; strncpy(tz, g.tm_zone ? g.tm_zone : "UTC", 7); memcpy(o + 16, tz, 8); uint32_t dst = g.tm_isdst > 0; int32_t gmt = (int32_t)g.tm_gmtoff; memcpy(o + 24, &dst, 4); memcpy(o + 28, &gmt, 4); }
    memcpy(c->out, o, sizeof o); c->n_out = 8; return 0;
}
static uint32_t acc_false(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }                            // 150 IsUserAccountSwitchLocked → false
RETURN_OBJECT(hid_create_applet_resource, "hid.IAppletResource")   // hid 0 CreateAppletResource
RETURN_OBJECT(hid_vibration_list, "hid.IActiveVibrationDeviceList")   // hid 203 CreateActiveVibrationDeviceList
// bsd:u 0 RegisterClient(配置, pid, 传输内存大小, 传输内存句柄) → u32（0 = 成功）| bsd:u 0 RegisterClient(config, pid, transfer memory size, transfer memory handle) → u32 (0 = success)
static uint32_t bsd_register_client(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }
// hid IAppletResource 0 GetSharedMemoryHandle：hid 共享内存 0x40000 字节（大小按 switchbrew）；全 0 = 没有输入设备，之后按「HID Shared Memory」页填 | hid IAppletResource 0 GetSharedMemoryHandle: 0x40000 bytes of hid shared memory (size per switchbrew); all zero = no input devices, filled later from the "HID Shared Memory" page
static uint32_t hid_shared_memory(b3_call_t *c) { static uint32_t h; if (!h) h = b3_shmem_new(0x40000, "hid"); c->copy[0] = h; c->n_copy = 1; return 0; }
// time:u 20 GetSharedMemoryNativeHandle：回 0x1000 字节共享内存的复制句柄（大小按 switchbrew）| time:u 20 GetSharedMemoryNativeHandle: returns a copy handle to 0x1000 bytes of shared memory (size per switchbrew)
uint32_t b3_shmem_new(uint64_t size, const char *tag);
// time:u 共享内存的时钟上下文（布局按公开资料：稳态时钟上下文 @0x0、本地系统时钟 @0x38、网络系统时钟 @0x80，各是"计数 u32 + 两份副本"的无锁结构，副本从 +8 起）。
// 全 0 时游戏算出的当前时间 = 0（存档时间显示 1970.1.1）。稳态时钟 = 客体时钟（与 GetSystemTick 同源），系统时钟偏移 = 宿主墙钟 − 稳态秒数；两者用同一个时钟源 ID
// Clock contexts in the time:u shared memory (layout per public documentation: steady clock context @0x0, local system clock @0x38, network system clock @0x80, each a lock-free "u32 counter + two copies" structure with the copies from +8).
// All zero makes the game's current time 0 (save timestamps show 1970-01-01). Steady clock = guest clock (same source as GetSystemTick); system clock offset = host wall clock − steady seconds; both use the same clock source ID
void b3_time_attach(void *base) {
    uint8_t *m = base; uint64_t src[2] = {0x6233746d63306b31ull, 0x7265766f6c636b31ull};   // 固定的时钟源 ID | fixed clock source ID
    int64_t steady = (int64_t)(guest_ns() / 1000000000ull); time_t now = time(NULL);
    for (int i = 0; i < 2; i++) {
        uint8_t *sc = m + 0x8 + i * 0x18; int64_t zero = 0; memcpy(sc, &zero, 8); memcpy(sc + 8, src, 16);   // SteadyClockContext{internal_offset ns, source}
        for (int k = 0; k < 2; k++) { uint8_t *c = m + (k ? 0x88 : 0x40) + i * 0x20; int64_t off = (int64_t)now - steady; memcpy(c, &off, 8); memcpy(c + 8, &steady, 8); memcpy(c + 16, src, 16); }   // SystemClockContext{offset s, {time_point s, source}}
    }
    LOG("B3-TIME 共享内存时钟：墙钟 %lld，稳态 %lld 秒 | shared-memory clocks: wall %lld, steady %lld s", (long long)now, (long long)steady, (long long)now, (long long)steady);
}
static uint32_t time_shared_memory(b3_call_t *c) { static uint32_t h; if (!h) h = b3_shmem_new(0x1000, "time"); c->copy[0] = h; c->n_copy = 1; return 0; }

// IWindowController 1 GetAppletResourceUserId：回 u64 ARUID。只有一个应用在跑，取固定的非 0 值 0x100（DECISION）
// IWindowController 1 GetAppletResourceUserId: returns a u64 ARUID. Only one application runs, so a fixed non-zero 0x100 is used (DECISION)
#define B3_ARUID 0x100
static uint32_t wc_get_aruid(b3_call_t *c) { c->out[0] = B3_ARUID; c->out[1] = 0; c->n_out = 2; return 0; }
static uint32_t ok_no_output(b3_call_t *c) { (void)c; return 0; }
// 返回一个新建事件的读端（复制句柄）；这类事件文档里只在状态变化时触发，此刻没有变化，所以不触发 | Return the read end of a new event (copy handle); per the docs these fire only on state changes, and none happens now, so it stays unsignaled
static uint32_t new_event_handle(b3_call_t *c) { uint32_t w; c->copy[0] = event_new(&w); c->n_copy = 1; return 0; }

// ---------- AppletMessage 队列（ICommonStateGetter 0 / 1）| AppletMessage queue (ICommonStateGetter 0 / 1) ----------
// 文档：应用启动后先收到 FocusStateChanged（0xF）；队列非空时 GetEventHandle 的事件处于触发状态；空队列 ReceiveMessage 回 0x680（AM 模块 128，描述 3）
// Docs: an application first receives FocusStateChanged (0xF) after start; the GetEventHandle event is signaled while the queue is non-empty; ReceiveMessage on an empty queue returns 0x680 (AM module 128, description 3)
static uint32_t g_msgq[16] = {0xF}, g_msgq_n = 1, g_msg_read, g_msg_write;
static pthread_mutex_t g_msgq_mu = PTHREAD_MUTEX_INITIALIZER;   // 前端线程（Home）与客体 IPC 都会动队列 | the front-end thread (Home) and guest IPC both touch the queue
static uint32_t g_focus = 1;   // 1 前台，2 后台（按 Home 切到主页）| 1 in focus, 2 out of focus (Home switched to the HOME menu)
// Home 键（前端调用）：Switch 上 Home 不是手柄按键，而是系统切到主页、应用收到 FocusStateChanged 并失去焦点；再按一次回到应用。DECISION：两次之间应用照常运行，由游戏自己决定是否暂停
// Home button (called by the front end): on Switch Home is not a gamepad button; the system switches to the HOME menu and the application receives FocusStateChanged and loses focus; pressing again returns. DECISION: the application keeps running in between; the game decides whether to pause
void b3_applet_home(void) {
    pthread_mutex_lock(&g_msgq_mu);
    g_focus = g_focus == 1 ? 2 : 1;
    if (g_msgq_n < 16) g_msgq[g_msgq_n++] = 0xF;
    if (g_msg_write) { uint64_t r[32] = {g_msg_write}; svc_thread(0x11, r); }
    pthread_mutex_unlock(&g_msgq_mu);
    LOG("B3-HOME 焦点 → %s | focus → %s", g_focus == 1 ? "前台" : "后台（主页）", g_focus == 1 ? "in focus" : "out of focus (HOME menu)");
}
static uint32_t csg_get_event_handle(b3_call_t *c) {
    pthread_mutex_lock(&g_msgq_mu);
    if (!g_msg_read) {
        g_msg_read = event_new(&g_msg_write);
        if (g_msgq_n) { uint64_t r[8] = {g_msg_write}; svc_thread(0x11, r); }   // 有消息就触发 | signal if there are messages
    }
    pthread_mutex_unlock(&g_msgq_mu);
    c->copy[0] = g_msg_read; c->n_copy = 1; return 0;   // 同一事件的读端每次都复制给客户端 | the same event's read end is copied to the client each time
}
static uint32_t csg_receive_message(b3_call_t *c) {
    pthread_mutex_lock(&g_msgq_mu);
    if (!g_msgq_n) { pthread_mutex_unlock(&g_msgq_mu); return 0x680; }
    c->out[0] = g_msgq[0]; c->n_out = 1;
    memmove(g_msgq, g_msgq + 1, --g_msgq_n * sizeof *g_msgq);
    if (!g_msgq_n && g_msg_write) { uint64_t r[32] = {g_msg_write}; svc_thread(0x12, r); }   // 队列空了就清触发 | clear the signal once the queue is empty
    pthread_mutex_unlock(&g_msgq_mu);
    return 0;
}
// 状态查询：GetOperationMode 5 → u8（0 掌机，DECISION：iPhone 按掌机），GetPerformanceMode 6 → u32（0 普通），GetCurrentFocusState 9 → u8（1 前台，按 Home 后 2）
// State queries: GetOperationMode 5 → u8 (0 handheld; DECISION: an iPhone counts as handheld), GetPerformanceMode 6 → u32 (0 normal), GetCurrentFocusState 9 → u8 (1 in focus, 2 after Home)
static uint32_t csg_operation_mode(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }
static uint32_t csg_performance_mode(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }
static uint32_t csg_focus_state(b3_call_t *c) { pthread_mutex_lock(&g_msgq_mu); c->out[0] = g_focus; pthread_mutex_unlock(&g_msgq_mu); c->n_out = 1; return 0; }

// ---------- apm ISession：0 SetPerformanceConfiguration(u32 模式, u32 配置)，1 GetPerformanceConfiguration(u32 模式) → u32；取回上次设的值（没设过为 0）
// ---------- apm ISession: 0 SetPerformanceConfiguration(u32 mode, u32 config), 1 GetPerformanceConfiguration(u32 mode) → u32; returns the last value set (0 if never set)
static uint32_t g_perf_config[2];
static uint32_t apm_set_config(b3_call_t *c) { if (c->n_in >= 2 && c->in[0] < 2) g_perf_config[c->in[0]] = c->in[1]; return 0; }
static uint32_t apm_get_config(b3_call_t *c) { c->out[0] = c->n_in >= 1 && c->in[0] < 2 ? g_perf_config[c->in[0]] : 0; c->n_out = 1; return 0; }

// ---------- 语言（set 与 IApplicationFunctions 21）| Language (set and IApplicationFunctions 21) ----------
// LanguageCode 是补 0 到 8 字节的字符串；可选语言表按 switchbrew「Settings services」的顺序（下标即 MakeLanguageCode 的参数）
// A LanguageCode is a string zero-padded to 8 bytes; the available-language table follows the order on the switchbrew page "Settings services" (the index is MakeLanguageCode's argument)
static const char *const g_langs[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static void lang_code(const char *s, uint32_t out[2]) { char b[8] = {0}; strncpy(b, s, 8); memcpy(out, b, 8); }
// 系统语言：环境变量 B3_LANGUAGE，缺省 zh-Hans（DECISION：用户用简体中文）| System language: environment variable B3_LANGUAGE, default zh-Hans (DECISION: the user reads Simplified Chinese)
static const char *system_language(void) { const char *e = getenv("B3_LANGUAGE"); return e && *e ? e : "zh-Hans"; }
static uint32_t af_desired_language(b3_call_t *c) { lang_code(system_language(), c->out); c->n_out = 2; return 0; }
static uint32_t set_language_code(b3_call_t *c) { lang_code(system_language(), c->out); c->n_out = 2; return 0; }
static uint32_t set_make_language_code(b3_call_t *c) {
    if (c->n_in < 1 || c->in[0] >= sizeof g_langs / sizeof *g_langs) return 0x3c69;   // set 的 InvalidLanguage（模块 105，描述 30）| set InvalidLanguage (module 105, description 30)
    lang_code(g_langs[c->in[0]], c->out); c->n_out = 2; return 0;
}
static uint32_t lang_list(b3_call_t *c, unsigned max) {   // 写进第 0 个输出缓冲，回 u32 个数 | written into output buffer 0, returns the u32 count
    b3_buf_t b = b3_out_buffer(c, 0); unsigned n = sizeof g_langs / sizeof *g_langs; if (n > max) n = max; if (n > b.size / 8) n = (unsigned)(b.size / 8);
    for (unsigned i = 0; i < n; i++) lang_code(g_langs[i], (uint32_t *)(uintptr_t)(b.addr + 8 * i));
    c->out[0] = n; c->n_out = 1; return 0;
}
static uint32_t set_languages(b3_call_t *c) { return lang_list(c, 15); }    // 1：老接口最多 15 个 | 1: the old interface returns at most 15
static uint32_t set_languages2(b3_call_t *c) { return lang_list(c, 18); }   // 5
static uint32_t set_language_count(b3_call_t *c) { c->out[0] = 15; c->n_out = 1; return 0; }
static uint32_t set_language_count2(b3_call_t *c) { c->out[0] = sizeof g_langs / sizeof *g_langs; c->n_out = 1; return 0; }

// ---------- hid IHidServer 的 Npad 设定（全部按 switchbrew「HID services」）：设定类无输出，查询类回上次设的值 | hid IHidServer Npad settings (all per switchbrew "HID services"): setters have no output, getters return the last value set
static uint32_t g_npad_style_set, g_npad_hold_type = 1;   // 握持方式未设时为 Horizontal（1）（文档：缺省横握）| hold type defaults to Horizontal (1) (docs: horizontal by default)
static uint32_t hid_set_style_set(b3_call_t *c) { if (c->n_in >= 1) g_npad_style_set = c->in[0]; return 0; }
static uint32_t hid_get_style_set(b3_call_t *c) { c->out[0] = g_npad_style_set; c->n_out = 1; return 0; }
static uint32_t hid_set_hold_type(b3_call_t *c) { if (c->n_in >= 4) g_npad_hold_type = c->in[2]; return 0; }   // (u64 aruid, u64 类型) | (u64 aruid, u64 type)
static uint32_t hid_get_hold_type(b3_call_t *c) { c->out[0] = g_npad_hold_type; c->out[1] = 0; c->n_out = 2; return 0; }
// ISelfController 9 GetLibraryAppletLaunchableEvent：回一个已触发的事件（DECISION：表示此刻可以启动库 applet；真要启动时那条命令会单独停下）
// ISelfController 9 GetLibraryAppletLaunchableEvent: returns a signaled event (DECISION: means library applets may be launched now; an actual launch stops on its own command)
static uint32_t sc_launchable_event(b3_call_t *c) { uint32_t w; c->copy[0] = event_new(&w); c->n_copy = 1; uint64_t r[8] = {w}; svc_thread(0x11, r); return 0; }

// fsp-srv 1005 GetGlobalAccessLogMode → u32（0 = 不记访问日志）| fsp-srv 1005 GetGlobalAccessLogMode → u32 (0 = no access logging)
static uint32_t fsp_access_log_mode(b3_call_t *c) { c->out[0] = 0; c->n_out = 1; return 0; }

// ---------- fsp-srv 的 RomFS 数据存储（200 OpenDataStorageByCurrentProcess → fs.IStorage：0 Read(u64 偏移, u64 大小, 输出缓冲)、4 GetSize → u64）
// ---------- fsp-srv RomFS data storage (200 OpenDataStorageByCurrentProcess → fs.IStorage: 0 Read(u64 offset, u64 size, output buffer), 4 GetSize → u64)
// 映像由 b3_romfs.c 从 $B3_INPUT/RomFS 虚拟出来；没有输入时回 fs 的 PathNotFound（模块 2，描述 1）| The image is virtualised by b3_romfs.c from $B3_INPUT/RomFS; with no input, fs PathNotFound (module 2, description 1)
typedef struct b3_romfs b3_romfs_t;
b3_romfs_t *b3_romfs_open(const char *root); uint64_t b3_romfs_size(const b3_romfs_t *r); uint64_t b3_romfs_read(b3_romfs_t *r, uint64_t off, void *buf, uint64_t n);
static b3_romfs_t *g_romfs;
static uint32_t fsp_open_data_storage(b3_call_t *c) {
    if (!g_romfs) {
        const char *in = getenv("B3_INPUT"); char root[4096];
        if (!in || !*in) { LOG("B3-ROMFS B3_INPUT 未设置 | B3_INPUT is not set"); return 0x202; }
        snprintf(root, sizeof root, "%s/RomFS", in);
        if (!(g_romfs = b3_romfs_open(root))) { LOG("B3-ROMFS 没有 RomFS 目录 | no RomFS directory"); return 0x202; }
    }
    strlcpy(c->obj, "fs.IStorage#romfs", sizeof c->obj); return 0;
}
static uint32_t fs_storage_read(b3_call_t *c) {
    if (!g_romfs || c->n_in < 4) return 0x202;
    uint64_t off = c->in[0] | (uint64_t)c->in[1] << 32, n = c->in[2] | (uint64_t)c->in[3] << 32; b3_buf_t b = b3_out_buffer(c, 0);
    if (n > b.size) n = b.size;
    b3_romfs_read(g_romfs, off, (void *)(uintptr_t)b.addr, n); return 0;
}
static uint32_t fs_storage_size(b3_call_t *c) { uint64_t n = g_romfs ? b3_romfs_size(g_romfs) : 0; c->out[0] = (uint32_t)n; c->out[1] = (uint32_t)(n >> 32); c->n_out = 2; return 0; }

const b3_cmd_t b3_applet_cmds[] = {
    {"appletOE", B3_CMIF, 0, oe_open_application_proxy, "switchbrew AM: IApplicationProxyService OpenApplicationProxy"},
    {"am.IApplicationProxy", B3_CMIF, 0, ap_common_state_getter, "switchbrew AM: GetCommonStateGetter"},
    {"am.IApplicationProxy", B3_CMIF, 1, ap_self_controller, "switchbrew AM: GetSelfController"},
    {"am.IApplicationProxy", B3_CMIF, 2, ap_window_controller, "switchbrew AM: GetWindowController"},
    {"am.IApplicationProxy", B3_CMIF, 3, ap_audio_controller, "switchbrew AM: GetAudioController"},
    {"am.IApplicationProxy", B3_CMIF, 4, ap_display_controller, "switchbrew AM: GetDisplayController"},
    {"am.IApplicationProxy", B3_CMIF, 11, ap_library_applet_creator, "switchbrew AM: GetLibraryAppletCreator"},
    {"am.IApplicationProxy", B3_CMIF, 20, ap_application_functions, "switchbrew AM: GetApplicationFunctions"},
    {"am.IApplicationProxy", B3_CMIF, 1000, ap_debug_functions, "switchbrew AM: GetDebugFunctions"},
    {"am.IWindowController", B3_CMIF, 1, wc_get_aruid, "switchbrew AM: IWindowController GetAppletResourceUserId"},
    {"am.IWindowController", B3_CMIF, 10, ok_no_output, "switchbrew AM: IWindowController AcquireForegroundRights"},
    {"am.ICommonStateGetter", B3_CMIF, 0, csg_get_event_handle, "switchbrew AM: ICommonStateGetter GetEventHandle"},
    {"am.ICommonStateGetter", B3_CMIF, 1, csg_receive_message, "switchbrew AM: ICommonStateGetter ReceiveMessage"},
    {"am.ICommonStateGetter", B3_CMIF, 5, csg_operation_mode, "switchbrew AM: ICommonStateGetter GetOperationMode"},
    {"am.ICommonStateGetter", B3_CMIF, 6, csg_performance_mode, "switchbrew AM: ICommonStateGetter GetPerformanceMode"},
    {"am.ICommonStateGetter", B3_CMIF, 9, csg_focus_state, "switchbrew AM: ICommonStateGetter GetCurrentFocusState"},
    {"am.ICommonStateGetter", B3_CMIF, 66, ok_no_output, "switchbrew AM: ICommonStateGetter SetCpuBoostMode"},
    {"am.IApplicationFunctions", B3_CMIF, 21, af_desired_language, "switchbrew AM: IApplicationFunctions GetDesiredLanguage"},
    {"bsd:u", B3_CMIF, 0, bsd_register_client, "switchbrew Sockets: IClient RegisterClient"},
    {"bsd:u", B3_CMIF, 1, ok_no_output, "switchbrew Sockets: IClient StartMonitoring"},
    {"hid", B3_CMIF, 0, hid_create_applet_resource, "switchbrew HID: IHidServer CreateAppletResource"},
    {"hid", B3_CMIF, 11, ok_no_output, "switchbrew HID: ActivateTouchScreen"},
    {"hid", B3_CMIF, 203, hid_vibration_list, "switchbrew HID: CreateActiveVibrationDeviceList"},
    {"time.ITimeZoneService", B3_CMIF, 101, tz_to_calendar, "switchbrew Glue/PSC: ITimeZoneService ToCalendarTimeWithMyRule"},
    {"prepo:u", B3_CMIF, 10105, ok_no_output, "switchbrew BCAT/prepo: SaveReportWithUser（游玩报告接受后丢弃 | play report accepted and dropped）"},
    {"acc:u0", B3_CMIF, 0, acc_user_count, "switchbrew Account: GetUserCount"},
    {"acc:u0", B3_CMIF, 1, acc_user_exists, "switchbrew Account: GetUserExistence"},
    {"acc:u0", B3_CMIF, 2, acc_list_users, "switchbrew Account: ListAllUsers"},
    {"acc:u0", B3_CMIF, 3, acc_list_users, "switchbrew Account: ListOpenUsers"},
    {"acc:u0", B3_CMIF, 4, acc_last_opened, "switchbrew Account: GetLastOpenedUser"},
    {"acc:u0", B3_CMIF, 101, acc_baas_manager, "switchbrew Account: GetBaasAccountManagerForApplication"},
    {"aoc:u", B3_CMIF, 50, ok_no_output, "switchbrew NS: IAddOnContentManager CheckAddOnContentMountStatus（没有追加内容 | no add-on content）"},
    {"pl:u", B3_CMIF, 0, ok_no_output, "switchbrew Shared Font: RequestLoad"},
    {"pl:u", B3_CMIF, 1, pl_load_state, "switchbrew Shared Font: GetLoadState"},
    {"pl:u", B3_CMIF, 2, pl_zero, "switchbrew Shared Font: GetSize（无共享字体 | no shared font）"},
    {"pl:u", B3_CMIF, 3, pl_zero, "switchbrew Shared Font: GetSharedMemoryAddressOffset（无共享字体 | no shared font）"},
    {"pl:u", B3_CMIF, 4, pl_shared_memory, "switchbrew Shared Font: GetSharedMemoryNativeHandle（全 0 | all zero）"},
    {"pl:u", B3_CMIF, 5, pl_font_order, "switchbrew Shared Font: GetSharedFontInOrderOfPriority（报 0 个 | reports 0 fonts）"},
    {"aoc:u", B3_CMIF, 2, aoc_count, "switchbrew NS: IAddOnContentManager CountAddOnContent"},
    {"acc:u0", B3_CMIF, 130, ok_no_output, "switchbrew Account: LoadOpenContext"},
    {"acc:u0", B3_CMIF, 131, acc_no_users, "switchbrew Account: ListOpenContextStoredUsers"},
    {"acc:u0", B3_CMIF, 150, acc_false, "switchbrew Account: IsUserAccountSwitchLocked"},
    {"am.IApplicationFunctions", B3_CMIF, 22, ok_no_output, "switchbrew AM: IApplicationFunctions SetTerminateResult"},
    {"acc:u0", B3_CMIF, 160, ok_no_output, "switchbrew Account: IAccountServiceForApplication InitializeApplicationInfoV2"},
    {"nifm:u", B3_CMIF, 5, nifm_general_service, "switchbrew NIFM: IStaticService CreateGeneralService"},
    {"nifm.IGeneralService", B3_CMIF, 4, nifm_create_request, "switchbrew NIFM: IGeneralService CreateRequest"},
    {"nifm.IRequest", B3_CMIF, 0, nifm_req_state, "switchbrew NIFM: IRequest GetRequestState（1 = Free，未接受 | not accepted）"},
    {"nifm.IRequest", B3_CMIF, 1, nifm_req_result, "switchbrew NIFM: IRequest GetResult（网络通信已禁用 | network communication disabled）"},
    {"nifm.IRequest", B3_CMIF, 2, nifm_req_events, "switchbrew NIFM: IRequest GetSystemEventReadableHandles"},
    {"nifm.IRequest", B3_CMIF, 21, nifm_req_applet_info, "switchbrew NIFM: IRequest GetAppletInfo（不弹小程序 | no applet）"},
    {"nifm.IRequest", B3_CMIF, 3, ok_no_output, "switchbrew NIFM: IRequest Cancel"},
    {"nifm.IRequest", B3_CMIF, 4, ok_no_output, "switchbrew NIFM: IRequest Submit"},
    {"nifm.IRequest", B3_CMIF, 6, ok_no_output, "switchbrew NIFM: IRequest SetRequirementPreset"},
    {"nifm.IRequest", B3_CMIF, 11, ok_no_output, "switchbrew NIFM: IRequest SetConnectionConfirmationOption"},
    {"hid", B3_CMIF, 201, ok_no_output, "switchbrew HID: SendVibrationValue (Mac 无震动，接受后丢弃，DECISION | no rumble on Mac, accepted and dropped, DECISION)"},
    {"hid", B3_CMIF, 21, ok_no_output, "switchbrew HID: ActivateMouse"},
    {"hid", B3_CMIF, 31, ok_no_output, "switchbrew HID: ActivateKeyboard"},
    {"hid", B3_CMIF, 66, ok_no_output, "switchbrew HID: StartSixAxisSensor"},
    {"hid.IActiveVibrationDeviceList", B3_CMIF, 0, ok_no_output, "switchbrew HID: IActiveVibrationDeviceList ActivateVibrationDevice"},
    {"fsp-srv", B3_CMIF, 1, ok_no_output, "switchbrew FS: IFileSystemProxy SetCurrentProcess"},
    {"fsp-srv", B3_CMIF, 200, fsp_open_data_storage, "switchbrew FS: IFileSystemProxy OpenDataStorageByCurrentProcess"},
    {"fs.IStorage", B3_CMIF, 0, fs_storage_read, "switchbrew FS: IStorage Read"},
    {"fs.IStorage", B3_CMIF, 4, fs_storage_size, "switchbrew FS: IStorage GetSize"},
    {"fsp-srv", B3_CMIF, 1005, fsp_access_log_mode, "switchbrew FS: IFileSystemProxy GetGlobalAccessLogMode"},
    {"hid", B3_CMIF, 100, hid_set_style_set, "switchbrew HID: SetSupportedNpadStyleSet"},
    {"hid", B3_CMIF, 101, hid_get_style_set, "switchbrew HID: GetSupportedNpadStyleSet"},
    {"hid", B3_CMIF, 102, ok_no_output, "switchbrew HID: SetSupportedNpadIdType"},
    {"hid", B3_CMIF, 109, ok_no_output, "switchbrew HID: ActivateNpadWithRevision"},
    {"hid", B3_CMIF, 120, hid_set_hold_type, "switchbrew HID: SetNpadJoyHoldType"},
    {"hid", B3_CMIF, 121, hid_get_hold_type, "switchbrew HID: GetNpadJoyHoldType"},
    {"hid", B3_CMIF, 128, ok_no_output, "switchbrew HID: SetNpadHandheldActivationMode"},
    {"am.ISelfController", B3_CMIF, 9, sc_launchable_event, "switchbrew AM: ISelfController GetLibraryAppletLaunchableEvent"},
    {"hid.IAppletResource", B3_CMIF, 0, hid_shared_memory, "switchbrew HID: IAppletResource GetSharedMemoryHandle"},
    {"apm.ISession", B3_CMIF, 0, apm_set_config, "switchbrew PPC: apm ISession SetPerformanceConfiguration"},
    {"apm.ISession", B3_CMIF, 1, apm_get_config, "switchbrew PPC: apm ISession GetPerformanceConfiguration"},
    {"set", B3_CMIF, 0, set_language_code, "switchbrew Settings: ISettingsServer GetLanguageCode"},
    {"set", B3_CMIF, 1, set_languages, "switchbrew Settings: ISettingsServer GetAvailableLanguageCodes"},
    {"set", B3_CMIF, 2, set_make_language_code, "switchbrew Settings: ISettingsServer MakeLanguageCode"},
    {"set", B3_CMIF, 3, set_language_count, "switchbrew Settings: ISettingsServer GetAvailableLanguageCodeCount"},
    {"set", B3_CMIF, 5, set_languages2, "switchbrew Settings: ISettingsServer GetAvailableLanguageCodes2"},
    {"set", B3_CMIF, 6, set_language_count2, "switchbrew Settings: ISettingsServer GetAvailableLanguageCodeCount2"},
    {"am.ICommonStateGetter", B3_CMIF, 900, ok_no_output, "switchbrew AM: ICommonStateGetter SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled"},
    {"am.ISelfController", B3_CMIF, 11, ok_no_output, "switchbrew AM: ISelfController SetOperationModeChangedNotification"},
    {"am.ISelfController", B3_CMIF, 12, ok_no_output, "switchbrew AM: ISelfController SetPerformanceModeChangedNotification"},
    {"am.ISelfController", B3_CMIF, 13, ok_no_output, "switchbrew AM: ISelfController SetFocusHandlingMode"},
    {"am.ISelfController", B3_CMIF, 14, ok_no_output, "switchbrew AM: ISelfController SetRestartMessageEnabled"},
    {"am.ISelfController", B3_CMIF, 130, ok_no_output, "switchbrew AM: ISelfController SetRecordVolumeMuted（过场时游戏静音录像；Mac 无录像，接受即可 | the game mutes recordings during cutscenes; no recording on Mac, accepted）"},
    {"am.ISelfController", B3_CMIF, 16, ok_no_output, "switchbrew AM: ISelfController SetOutOfFocusSuspendingEnabled"},
    {"pctl:s", B3_CMIF, 0, pctl_create_service, "switchbrew PCTL: CreateService"},
    {"pctl:s", B3_CMIF, 1, pctl_create_service, "switchbrew PCTL: CreateServiceWithoutInitialize"},
    {"pctl:a", B3_CMIF, 0, pctl_create_service, "switchbrew PCTL: CreateService"},
    {"pctl:a", B3_CMIF, 1, pctl_create_service, "switchbrew PCTL: CreateServiceWithoutInitialize"},
    {"pctl:r", B3_CMIF, 0, pctl_create_service, "switchbrew PCTL: CreateService"},
    {"pctl:r", B3_CMIF, 1, pctl_create_service, "switchbrew PCTL: CreateServiceWithoutInitialize"},
    {"pctl.IParentalControlService", B3_CMIF, 1, ok_no_output, "switchbrew PCTL: IParentalControlService Initialize"},
    {"apm", B3_CMIF, 0, apm_open_session, "switchbrew PPC: apm IManager OpenSession"},
    {"time:u", B3_CMIF, 3, time_timezone_service, "switchbrew Time: IStaticService GetTimeZoneService"},
    {"time:u", B3_CMIF, 20, time_shared_memory, "switchbrew Time: IStaticService GetSharedMemoryNativeHandle"},
    {"pctl", B3_CMIF, 0, pctl_create_service, "switchbrew PCTL: IParentalControlServiceFactory CreateService"},
    {"pctl", B3_CMIF, 1, pctl_create_service, "switchbrew PCTL: IParentalControlServiceFactory CreateServiceWithoutInitialize"},
    {"am.ISelfController", B3_CMIF, 91, new_event_handle, "switchbrew AM: ISelfController GetAccumulatedSuspendedTickChangedEvent"},
    {"am.IApplicationFunctions", B3_CMIF, 130, new_event_handle, "switchbrew AM: IApplicationFunctions GetGpuErrorDetectedSystemEvent"},
};
const unsigned b3_applet_ncmds = sizeof b3_applet_cmds / sizeof *b3_applet_cmds;
