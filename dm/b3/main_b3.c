// B3 宿主的最小入口：装模块 → SDK 运行时初始化 → 各模块 init_array → nnMain，顺序照抄 dm/host/main.c 的 guest_main / host_preload。
// 不调任何通用层游戏功能（dm_padmode_boot、dm_cloud_boot、存档节流、固定种子、触屏选怪、TACT 预取等都不进来），不开窗口（无前端，客体在当前线程跑）。
// 还定义 dm/host/main.c 与 diag.c 原本提供、被复用文件引用的全局：g_flag、g_t0_ns、dm_prof_mark、dm_prof_add（耦合清单见 coupling.tsv）。
// 用法：b3host <产物目录> [模块...] [-- 游戏参数...]；不给模块时按目录里实有的产物取 main、subsdk0..9、sdk（Horizon 的装载顺序，rtld 的职责由宿主承担）；不给参数打印用法、退出码 3。
// Minimal entry of the B3 host: load modules → SDK runtime init → each module's init_array → nnMain, in the order copied from guest_main / host_preload in dm/host/main.c.
// Calls no the base runtime game feature (dm_padmode_boot, dm_cloud_boot, save throttling, fixed seed, touch targeting, TACT prefetch etc. are all left out) and opens no window (no front end, the guest runs on the current thread).
// Also defines the globals that dm/host/main.c and diag.c provide and the reused files reference: g_flag, g_t0_ns, dm_prof_mark, dm_prof_add (coupling inventory in coupling.tsv).
// Usage: b3host <build-output dir> [modules...] [-- game arguments...]; with no modules, takes main, subsdk0..9, sdk as present in the directory (Horizon load order; the host plays rtld's role); with no arguments prints usage and exits with code 3.
#include "dm.h"
#include <stdatomic.h>
#include <unistd.h>

void stubs_report(void); void crash_install(void); void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void);
void guest_thread_init(void); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n) { void *p = guest_sym(n); if (!p) die("missing guest symbol %s", n); return p; }

// 游戏参数：argv[0] 固定为 "B3"，"--" 之后的原样追加（经 nn::os::SetHostArgc / SetHostArgv 交给 SDK）
// Game arguments: argv[0] is fixed to "B3", whatever follows "--" is appended as is (handed to the SDK through nn::os::SetHostArgc / SetHostArgv)
static char *g_argv[64] = {"B3"}; static int g_argc = 1;

// ---------- 被复用文件引用、原由通用层 main.c / diag.c 定义的全局 ----------
// ---------- Globals referenced by the reused files, originally defined by the base runtime main.c / diag.c ----------
struct dm_flags g_flag;
uint64_t g_t0_ns;
// ProgramId（svc GetInfo 18，hle_svc.c 的 DM_B3 钩子读它）：暂从环境变量 B3_PROGRAM_ID（十六进制 title id）读，以后由 profile 设置；0 = 未设置，客体查询时 abort
// ProgramId (svc GetInfo 18, read by the DM_B3 hook in hle_svc.c): read from the environment variable B3_PROGRAM_ID (hex title id) for now, later set from the profile; 0 = not set, abort when the guest queries it
uint64_t b3_program_id;
static void program_id_init(void) {
    const char *e = getenv("B3_PROGRAM_ID"); if (!e || !*e) return;
    char *end; unsigned long long v = strtoull(e, &end, 16);
    if (*end || !v) die("B3_PROGRAM_ID 不是非零的十六进制数：%s | B3_PROGRAM_ID is not a non-zero hex number: %s", e, e);
    b3_program_id = v; LOG("B3 ProgramId %#018llx", v);
}
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.movie = !!getenv("DM_MOVIE"); g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}
// 启动时间线（DM_BOOTPROF=1）：分段打点只记一行日志；计数器只累加，不出报表（B3 的报表格式等 定）
// Boot timeline (DM_BOOTPROF=1): a segment mark only logs one line; counters only accumulate, no report (the B3 report format waits for )
static _Atomic uint64_t g_prof_ns[PROF_N], g_prof_bytes[PROF_N];
void dm_prof_add(int id, uint64_t ns, uint64_t bytes) {
    if (id < 0 || id >= PROF_N) die("dm_prof_add: id %d out of range", id);
    atomic_fetch_add(&g_prof_ns[id], ns); atomic_fetch_add(&g_prof_bytes[id], bytes);
}
void dm_prof_mark(const char *seg) {
    if (g_flag.bootprof) LOG("BOOTPROF %.1f ms %s", (now_ns() - g_t0_ns) / 1e6, seg);
}

// 客体主线程：照抄 nn::init::Start 的顺序（细节与依据见 dm/host/main.c guest_main 的注释）
// Guest main thread: copies the order of nn::init::Start (details and evidence in the comments of guest_main in dm/host/main.c)
static void guest_main(void) {
    guest_thread_init();
    uint32_t hmain = svc_thread_main_init();   // 主线程句柄 = nnosInitialize 的 x0 | main thread handle = x0 of nnosInitialize
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    dm_prof_mark("2 SDK init (nnosInitialize -> nninitStartup)");
    run_module_inits();   // 各模块 DT_INIT（全局构造函数） | each module's DT_INIT (global constructors)
    dm_prof_mark("3 module init_array");
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    LOG(">>> nnMain"); ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}

// 产物目录里实有的模块，按 Horizon 装载顺序：main、subsdk0..9、sdk | Modules present in the build directory, in Horizon load order: main, subsdk0..9, sdk
static int default_modules(const char *dir, const char **names) {
    static char sub[10][16]; int n = 0; char path[1024];
    const char *order[12] = {"main"};
    for (int i = 0; i < 10; i++) { snprintf(sub[i], sizeof sub[i], "subsdk%d", i); order[1 + i] = sub[i]; }
    order[11] = "sdk";
    for (int i = 0; i < 12 && n < MAX_MOD; i++) {
        snprintf(path, sizeof path, "%s/%s.patched.bin", dir, order[i]);
        if (!access(path, R_OK)) names[n++] = order[i];
    }
    if (!n) die("%s 里没有 *.patched.bin 产物 | no *.patched.bin outputs in %s", dir, dir);
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: b3host <build-output dir> [module...] [-- game args...]\n"
                        "  loads <dir>/<module>.patched.bin (default: main, subsdk0..9, sdk present in <dir>) and runs nnMain\n");
        return 3;
    }
    g_t0_ns = now_ns();
    // 出图间隔按游戏设的走（/过场动画太快、音画不同步）：B3 过场把出图间隔设成 2（30 帧），过场按帧推进；
    //通用层有窗口时缺省的 display 模式不管间隔、每节拍出一帧，过场就跑成 60 帧、快一倍。DM_PRESENT_INTERVAL=display 可改回
    // Present interval follows the game (/cutscenes too fast, audio out of sync): B3 cutscenes set the present interval to 2 (30 fps) and advance per frame;
    // the base runtime's default display mode with a window ignores the interval and presents every tick, so cutscenes ran at 60 fps, twice as fast. DM_PRESENT_INTERVAL=display switches back
    setenv("DM_PRESENT_INTERVAL", "game", 0);
    flags_init();
    program_id_init();
    dm_prof_mark("0 host main");
    crash_install();
    crash_set_dump_hook(svc_thread_dump);   // 崩溃时导出客体线程现场 | dump guest thread state on a crash
    const char *names[MAX_MOD]; int n = 0, i = 2;
    for (; i < argc && strcmp(argv[i], "--") && n < MAX_MOD; i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    if (!n) n = default_modules(argv[1], names);
    load_modules(argv[1], names, n);
    stubs_report();
    dm_prof_mark("1 host load (NSO / relocation / forced takeover)");
    void b3_front_run(void (*guest)(void)); b3_front_run(guest_main);   // b3_front.c / b3_front_mac.m：B3_WINDOW=1 时开窗口、客体另起线程 | opens a window and moves the guest to its own thread when B3_WINDOW=1
    return 0;
}
