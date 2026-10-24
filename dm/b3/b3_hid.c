// B3 的 HID 共享内存：在 hid 共享内存（0x40000 字节）的 Npad 第 8 项（掌机，HidNpadIdType_Handheld）写入一个已连接的掌机模式手柄，并以 60 Hz 往掌机 LIFO 里推状态。
// 布局与语义取自公开资料：switchbrew「HID Shared Memory」页与 libnx 的 nx/include/switch/services/hid.h、nx/source/services/hid.c 的 LIFO 读法：
// 新条目写在 tail+1，条目的 sampling_number 与状态里的 sampling_number 相同且逐个加 1，count 最多 17。
// 宿主输入接口 b3_input_set：以后由 macOS / iOS 的 GameController 前端调用；现在没有前端，按键全松、摇杆居中。
// B3 HID shared memory: writes a connected handheld-mode controller into Npad entry 8 (handheld, HidNpadIdType_Handheld) of the hid shared memory (0x40000 bytes) and pushes states into the handheld LIFO at 60 Hz.
// Layout and semantics come from public sources: the switchbrew page "HID Shared Memory" and libnx's nx/include/switch/services/hid.h , plus the LIFO read logic in nx/source/services/hid.c:
// a new entry goes to tail+1, the entry's sampling_number equals the state's sampling_number and increases by 1 each time, count is at most 17.
// Host input interface b3_input_set: to be called later by a macOS / iOS GameController front end; with no front end yet, all buttons are released and sticks centred.
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include "dm.h"
#include <stdatomic.h>

#define NPAD_BASE 0x9a00u            // HidSharedMemory.npad
#define NPAD_ENTRY 0x5000u           // 每个 Npad 项 | per Npad entry
#define NPAD_HANDHELD 8              // libnx _hidGetNpadInternalState：Handheld → entries[8]
#define OFF_STYLE_SET 0x0
#define OFF_JOY_ASSIGNMENT 0x4
#define OFF_FULL_KEY_COLOR 0x8       // {attribute, main, sub}
#define OFF_JOY_COLOR 0x14           // {attribute, left main, left sub, right main, right sub}
#define OFF_HANDHELD_LIFO 0x378      // HidNpadCommonLifo：头 0x20 + 17 × 0x30 | header 0x20 + 17 × 0x30
#define OFF_DEVICE_TYPE 0x4188
#define OFF_SYSTEM_PROPERTIES 0x4190
#define OFF_BATTERY_LEVEL 0x419c     // u32 × 3
#define STYLE_HANDHELD (1u << 1)     // HidNpadStyleTag_NpadHandheld
#define DEVICE_HANDHELD ((1u << 2) | (1u << 3))   // HidDeviceTypeBits_HandheldLeft | HandheldRight
#define ATTR_HANDHELD 0x3f           // IsConnected | IsWired | IsLeft/RightConnected | IsLeft/RightWired
// 系统属性：IsAbxyButtonOriented（11）、IsPlusAvailable（13）、IsMinusAvailable（14）、IsDirectionalButtonsAvailable（15）| system properties
#define SYSPROP_HANDHELD ((1ull << 11) | (1ull << 13) | (1ull << 14) | (1ull << 15))
#define GRAY 0xff828282u             // 灰色 RGBA（DECISION：颜色只影响界面显示）| grey RGBA (DECISION: colour only affects UI)

static _Atomic uint64_t g_buttons;   // HidNpadButton 位 | HidNpadButton bits
static _Atomic int32_t g_stick[4];   // lx, ly, rx, ry（±0x7fff）

// 触屏：hid 共享内存 0x400 处的 HidTouchScreenLifo（libnx hid.h：头 0x20 + 17 × {u64 采样号, HidTouchScreenState 0x290}），
// HidTouchScreenState = {u64 采样号, s32 点数, u32 保留, HidTouchState × 16}，HidTouchState（0x28）= {u64 delta_time, u32 属性（位 0 Start、位 1 End）, u32 手指号, u32 x, u32 y, u32 直径 x, u32 直径 y, u32 角度, u32 保留}；坐标是 1280×720 屏幕点。
// Touch screen : the HidTouchScreenLifo at 0x400 of the hid shared memory (libnx hid.h: header 0x20 + 17 × {u64 sampling number, HidTouchScreenState 0x290}),
// HidTouchScreenState = {u64 sampling number, s32 count, u32 reserved, HidTouchState × 16}, HidTouchState (0x28) = {u64 delta_time, u32 attributes (bit 0 Start, bit 1 End), u32 finger id, u32 x, u32 y, u32 diameter x, u32 diameter y, u32 angle, u32 reserved}; coordinates are 1280×720 screen points.
#define TOUCH_LIFO 0x400u
#define TOUCH_MAX 16
typedef struct { uint32_t id, x, y, attr; } b3_touch_t;
static b3_touch_t g_touch[TOUCH_MAX]; static int g_ntouch; static uint64_t g_touch_sampling;
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_push_lock = PTHREAD_MUTEX_INITIALIZER;   // 定时线程与输入变化都会推样本，串行化 | the timer thread and input changes both push samples; serialize them
static uint8_t *g_hid;
static uint64_t g_sampling;
static void push_now(void);
// ---------- 输入录制 / 回放（性能基准用）----------
// B3_INPUT_REC=<文件>：前端输入每变一次记一行「读取段 帧 按键 lx ly rx ry」；帧 = 该段读取画面结束后出图的第几帧。
// B3_INPUT_PLAY=<文件>：忽略前端输入，按同样的“段 + 帧”放回；放完记 B3-REPLAY-END，B3_REPLAY_EXIT=1 时再过 120 帧退出。
// ---------- Input record / replay (for benchmarks) ----------
// B3_INPUT_REC=<file>: one line "epoch frame buttons lx ly rx ry" per front-end input change; frame = presents since that epoch's loading screen ended.
// B3_INPUT_PLAY=<file>: ignore front-end input and play the lines back by the same "epoch + frame"; when done log B3-REPLAY-END, and with B3_REPLAY_EXIT=1 exit 120 frames later.
__attribute__((weak)) _Atomic uint32_t g_b3_load_epoch;   // hle_nvn.c；单测程序里没有 | hle_nvn.c; absent in test programs
__attribute__((weak)) _Atomic uint64_t g_b3_frame_now;    // b3_nvn.c
static int g_replaying;
static uint64_t epoch_frame(uint32_t *ep) {   // 当前读取段与段内帧号 | current epoch and frame within it
    static uint32_t last_ep; static uint64_t base;
    uint32_t e = atomic_load(&g_b3_load_epoch); uint64_t f = atomic_load(&g_b3_frame_now);
    if (e != last_ep) { last_ep = e; base = f; }
    *ep = e; return f - base;
}
static void rec_input(uint64_t b, const int32_t s[4]) {
    static FILE *fp; static int init; static uint64_t lb; static int32_t ls[4];
    if (!init) { init = 1; const char *p = getenv("B3_INPUT_REC"); if (p && *p) { fp = fopen(p, "w"); if (fp) LOG("B3-REC 录制输入到 %s | recording input", p); } }
    if (!fp || (b == lb && !memcmp(s, ls, sizeof ls))) return;
    uint32_t e; uint64_t f = epoch_frame(&e);
    fprintf(fp, "%u\t%llu\t%llx\t%d\t%d\t%d\t%d\n", e, (unsigned long long)f, (unsigned long long)b, s[0], s[1], s[2], s[3]); fflush(fp);
    lb = b; memcpy(ls, s, sizeof ls);
}
void b3_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    { int32_t s[4] = {lx, ly, rx, ry}; rec_input(buttons, s); }
    if (g_replaying) return;   // 回放时前端输入不进游戏 | during replay front-end input does not reach the game
    uint64_t old = atomic_exchange(&g_buttons, buttons);
    atomic_store(&g_stick[0], lx); atomic_store(&g_stick[1], ly); atomic_store(&g_stick[2], rx); atomic_store(&g_stick[3], ry);
    if (old != buttons) push_now();   // 按键变化立即推一个样本，不等下一个 60 Hz 节拍（少一次量化延迟）| push a sample as soon as buttons change instead of waiting for the next 60 Hz tick (one less quantization delay)
}

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// 往一个 HidNpadCommonLifo 推一条状态 | Push one state into a HidNpadCommonLifo
void b3_hid_push(uint8_t *lifo, uint64_t sampling, uint64_t buttons, const int32_t stick[4], uint32_t attributes) {
    _Atomic uint64_t *hdr = (_Atomic uint64_t *)lifo;   // unused, buffer_count, tail, count
    uint64_t tail = (atomic_load(&hdr[2]) + 1) % 17;
    uint8_t *e = lifo + 0x20 + tail * 0x30, st[0x28] = {0};
    w64(st, sampling); w64(st + 8, buttons);
    w32(st + 0x10, (uint32_t)stick[0]); w32(st + 0x14, (uint32_t)stick[1]); w32(st + 0x18, (uint32_t)stick[2]); w32(st + 0x1c, (uint32_t)stick[3]);
    w32(st + 0x20, attributes);
    atomic_store((_Atomic uint64_t *)e, sampling);
    memcpy(e + 8, st, sizeof st);
    atomic_store(&hdr[1], 17);
    atomic_store(&hdr[2], tail);
    uint64_t n = atomic_load(&hdr[3]); if (n < 17) atomic_store(&hdr[3], n + 1);
}

// 写一个掌机模式 Npad 项的静态字段 | Write the static fields of a handheld-mode Npad entry
void b3_hid_fill_handheld(uint8_t *npad) {
    w32(npad + OFF_JOY_ASSIGNMENT, 0);                                    // Dual
    w32(npad + OFF_FULL_KEY_COLOR, 2);                                    // NoController：掌机没有 FullKey | NoController: no FullKey in handheld
    w32(npad + OFF_JOY_COLOR, 0); for (int i = 0; i < 4; i++) w32(npad + OFF_JOY_COLOR + 4 + 4 * i, GRAY);
    w32(npad + OFF_DEVICE_TYPE, DEVICE_HANDHELD);
    w64(npad + OFF_SYSTEM_PROPERTIES, SYSPROP_HANDHELD);
    for (int i = 0; i < 3; i++) w32(npad + OFF_BATTERY_LEVEL + 4 * i, 4);
    w32(npad + OFF_STYLE_SET, STYLE_HANDHELD);                            // 最后写样式，读者看到样式时其余字段已在 | style last, so readers that see it find the other fields in place
}

// 前端设当前触点（ids 唯一；x / y 已换算到 1280×720）。新出现的手指带 Start，消失的手指补一个带 End 的样本 | front end sets the current touches (unique ids; x / y already scaled to 1280×720). New fingers carry Start; fingers that disappear get one sample with End
static void touch_push_locked(const b3_touch_t *t, int n) {
    uint8_t *lifo = g_hid + TOUCH_LIFO; _Atomic uint64_t *hdr = (_Atomic uint64_t *)lifo;
    uint64_t tail = (atomic_load(&hdr[2]) + 1) % 17, smp = ++g_touch_sampling;
    uint8_t *e = lifo + 0x20 + tail * 0x298, st[0x290] = {0};
    w64(st, smp); w32(st + 8, (uint32_t)n);
    for (int i = 0; i < n; i++) { uint8_t *q = st + 0x10 + i * 0x28;
        w32(q + 8, t[i].attr); w32(q + 0xc, t[i].id); w32(q + 0x10, t[i].x); w32(q + 0x14, t[i].y); w32(q + 0x18, 15); w32(q + 0x1c, 15); }
    atomic_store((_Atomic uint64_t *)e, smp); memcpy(e + 8, st, sizeof st);
    atomic_store(&hdr[1], 17); atomic_store(&hdr[2], tail);
    uint64_t c = atomic_load(&hdr[3]); if (c < 17) atomic_store(&hdr[3], c + 1);
}
void b3_touch_set(int n, const uint32_t *ids, const float *xs, const float *ys) {
    if (n > TOUCH_MAX) n = TOUCH_MAX;
    pthread_mutex_lock(&g_touch_lock);
    b3_touch_t nw[TOUCH_MAX], gone[TOUCH_MAX]; int ng = 0;
    for (int i = 0; i < n; i++) {
        float x = xs[i] < 0 ? 0 : xs[i] > 1279 ? 1279 : xs[i], y = ys[i] < 0 ? 0 : ys[i] > 719 ? 719 : ys[i];
        nw[i] = (b3_touch_t){ids[i], (uint32_t)x, (uint32_t)y, 1};
        for (int j = 0; j < g_ntouch; j++) if (g_touch[j].id == ids[i]) nw[i].attr = 0;
    }
    for (int j = 0; j < g_ntouch; j++) { int still = 0; for (int i = 0; i < n; i++) still |= ids[i] == g_touch[j].id; if (!still) { gone[ng] = g_touch[j]; gone[ng++].attr = 2; } }
    if (g_hid && ng) {   // 抬起：先推一个含 End 手指的样本 | lift: first push a sample that contains the ended fingers
        b3_touch_t all[TOUCH_MAX * 2]; int k = 0; for (int i = 0; i < n; i++) all[k++] = nw[i]; for (int i = 0; i < ng && k < TOUCH_MAX; i++) all[k++] = gone[i];
        touch_push_locked(all, k);
    }
    memcpy(g_touch, nw, sizeof(b3_touch_t) * (size_t)n); g_ntouch = n;
    if (g_hid) touch_push_locked(g_touch, g_ntouch);
    for (int i = 0; i < g_ntouch; i++) g_touch[i].attr = 0;   // Start 只报一次 | Start is reported once
    { static int nlog; if (nlog < 200 && (n || ng)) { nlog++; LOG("B3-TOUCH n=%d%s x=%u y=%u", n, ng ? " (抬起 | lift)" : "", n ? nw[0].x : gone[0].x, n ? nw[0].y : gone[0].y); } }
    pthread_mutex_unlock(&g_touch_lock);
}

static void push_now(void) {
    if (!g_hid) return;
    pthread_mutex_lock(&g_push_lock);
    uint8_t *npad = g_hid + NPAD_BASE + NPAD_HANDHELD * NPAD_ENTRY;
    int32_t s[4]; for (int i = 0; i < 4; i++) s[i] = atomic_load(&g_stick[i]);
    b3_hid_push(npad + OFF_HANDHELD_LIFO, ++g_sampling, atomic_load(&g_buttons), s, ATTR_HANDHELD);
    pthread_mutex_unlock(&g_push_lock);
}
static void *hid_thread(void *arg) {
    (void)arg;
    for (;;) { push_now(); pthread_mutex_lock(&g_touch_lock); touch_push_locked(g_touch, g_ntouch); pthread_mutex_unlock(&g_touch_lock); struct timespec ts = {0, 16666667}; nanosleep(&ts, NULL); }
    return NULL;
}

// b3_kernel.c 在 tag 为 "hid" 的共享内存映射后调用 | Called by b3_kernel.c after shared memory tagged "hid" is mapped
// 脚本按键（自动化运行用）：B3_PRESS="A@25,A@35.5,B@40:300"——在 hid 共享内存挂上后第 N 秒按下该键 150 ms（可用 :毫秒 改）。键名按 libnx HidNpadButton 的位：
// A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN，另有 HOME（切主页 / 回游戏）。与 b3_input_set 的前端输入按位或（这里只加、只放自己按下的位）。
// Scripted presses (for automated runs): B3_PRESS="A@25,A@35.5,B@40:300" presses the button for 150 ms (or :ms) at second N after hid shared memory is attached. Names follow the bits of libnx HidNpadButton:
// A B X Y LS RS L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN, plus HOME (toggle HOME menu / game). OR-ed with the front end's input from b3_input_set (only its own bits are set and released here).
static const char *const k_btn_names[] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
int b3_button_bit(const char *name) { for (int i = 0; i < 16; i++) if (!strcasecmp(name, k_btn_names[i])) return i; return -1; }
static void *press_thread(void *arg) {
    const char *spec = arg; struct { double at; int bit, ms; } ev[64]; int n = 0;
    for (const char *p = spec; *p && n < 64; ) {
        char name[8] = {0}; int k = 0; while (*p && *p != '@' && k < 7) name[k++] = *p++;
        if (*p != '@') break;
        double at = strtod(p + 1, (char **)&p); int bit = !strcasecmp(name, "HOME") ? 16 : b3_button_bit(name);   // HOME：切主页 / 回游戏（不是手柄位）| HOME: toggle HOME menu / game (not a gamepad bit)
        if (bit < 0) die("B3_PRESS：不认识的键 %s | unknown button", name);
        int ms = 150; if (*p == ':') ms = (int)strtol(p + 1, (char **)&p, 10);   // 可选 :毫秒 指定按住时长| optional :ms gives the hold time 
        ev[n].at = at; ev[n].bit = bit; ev[n].ms = ms > 0 ? ms : 150; n++;
        while (*p == ',' || *p == ' ') p++;
    }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n; i++) {
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (double)(t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9; if (el >= ev[i].at) break; usleep(5000); }
        if (ev[i].bit == 16) { void b3_applet_home(void); b3_applet_home(); LOG("B3-PRESS HOME @%.1fs", ev[i].at); continue; }
        atomic_fetch_or(&g_buttons, 1ull << ev[i].bit); LOG("B3-PRESS %s @%.1fs", k_btn_names[ev[i].bit], ev[i].at);
        usleep((useconds_t)ev[i].ms * 1000); atomic_fetch_and(&g_buttons, ~(1ull << ev[i].bit));
    }
    return NULL;
}

// 随机输入（自动长测用）：B3_MONKEY=开始秒数[,种子]——从该秒起每 120–400 ms 随机推左摇杆、随机按攻击 / 跳跃 / 闪避类键（A B X Y ZR ZL R），偶尔推右摇杆；
// 不按 + / − / Home，避免停在菜单。与 B3_PRESS、前端输入按位或
// Random input (for automated soak runs): B3_MONKEY=start_seconds[,seed] - from that second on, every 120-400 ms push the left stick randomly and press attack / jump / dodge buttons (A B X Y ZR ZL R) at random, sometimes the right stick;
// never + / − / Home, so it does not get stuck in menus. OR-ed with B3_PRESS and the front end's input
static void *monkey_thread(void *arg) {
    const char *spec = arg; double start = strtod(spec, NULL); const char *c = strchr(spec, ','); uint32_t seed = c ? (uint32_t)strtoul(c + 1, NULL, 10) : 1;
    sleep((unsigned)(start > 0 ? start : 0)); LOG("B3-MONKEY 开始（种子 %u）| started (seed %u)", seed, seed);
    static const int pool[] = {0, 1, 2, 3, 9, 8, 7}; uint64_t held = 0;
    for (;;) {
        seed = seed * 1103515245u + 12345u; uint32_t r = seed >> 8;
        atomic_fetch_and(&g_buttons, ~held); held = 0;
        if (r % 100 < 70) held |= 1ull << pool[(r >> 7) % 7];
        if (r % 100 < 15) held |= 1ull << pool[(r >> 11) % 7];
        atomic_fetch_or(&g_buttons, held);
        double ang = ((r >> 14) % 360) * 3.14159265 / 180; int on = (r >> 23) % 4 != 0;
        atomic_store(&g_stick[0], on ? (int32_t)(cos(ang) * 32767) : 0); atomic_store(&g_stick[1], on ? (int32_t)(sin(ang) * 32767) : 0);
        atomic_store(&g_stick[2], (r >> 25) % 6 == 0 ? (int32_t)(((int)(r % 3) - 1) * 32767) : 0); atomic_store(&g_stick[3], 0);
        push_now(); usleep(120000 + (r % 280) * 1000);
    }
    return NULL;
}

static void *replay_thread(void *arg) {
    FILE *fp = fopen((const char *)arg, "r"); if (!fp) die("B3_INPUT_PLAY 打不开 %s | cannot open", (const char *)arg);
    unsigned e; unsigned long long f, b; int s0, s1, s2, s3, n = 0;
    while (fscanf(fp, "%u\t%llu\t%llx\t%d\t%d\t%d\t%d", &e, &f, &b, &s0, &s1, &s2, &s3) == 7) {
        for (;;) { uint32_t ce; uint64_t cf = epoch_frame(&ce); if (ce > e || (ce == e && cf >= f)) break; usleep(1000); }
        atomic_store(&g_buttons, b); atomic_store(&g_stick[0], s0); atomic_store(&g_stick[1], s1); atomic_store(&g_stick[2], s2); atomic_store(&g_stick[3], s3);
        push_now(); n++;
    }
    fclose(fp);
    LOG("B3-REPLAY-END 放完 %d 条，帧 %llu | replayed %d entries, frame %llu", n, (unsigned long long)atomic_load(&g_b3_frame_now), n, (unsigned long long)atomic_load(&g_b3_frame_now));
    const char *x = getenv("B3_REPLAY_EXIT");
    if (x && !strcmp(x, "1")) { uint64_t end = atomic_load(&g_b3_frame_now) + 120; while (atomic_load(&g_b3_frame_now) < end) usleep(10000); LOG("B3-REPLAY-EXIT"); exit(0); }
    return NULL;
}

void b3_hid_attach(void *base) {
    if (g_hid) return;
    g_hid = base;
    uint8_t *npad = g_hid + NPAD_BASE + NPAD_HANDHELD * NPAD_ENTRY;
    b3_hid_fill_handheld(npad);
    int32_t s[4] = {0}; b3_hid_push(npad + OFF_HANDHELD_LIFO, ++g_sampling, 0, s, ATTR_HANDHELD);
    pthread_t t; if (pthread_create(&t, NULL, hid_thread, NULL)) die("B3 HID 线程创建失败 | B3 HID thread creation failed");
    pthread_detach(t);
    LOG("B3-HID attach npad=handheld style=%#x device=%#x", STYLE_HANDHELD, DEVICE_HANDHELD);
    { const char *m = getenv("B3_MONKEY"); if (m && *m) { pthread_t mt; if (pthread_create(&mt, NULL, monkey_thread, (void *)m)) die("B3_MONKEY 线程创建失败 | thread creation failed"); pthread_detach(mt); } }
    { const char *pl = getenv("B3_INPUT_PLAY"); if (pl && *pl) { g_replaying = 1; pthread_t rt; if (pthread_create(&rt, NULL, replay_thread, (void *)pl)) die("B3_INPUT_PLAY 线程创建失败 | thread creation failed"); pthread_detach(rt); LOG("B3-REPLAY 回放输入 %s | replaying input", pl); } }
    { const char *spec = getenv("B3_PRESS"); if (spec && *spec) { pthread_t pt; if (pthread_create(&pt, NULL, press_thread, (void *)spec)) die("B3_PRESS 线程创建失败 | thread creation failed"); pthread_detach(pt); } }
}
