//通用层耦合桩：被复用的通用层通用源码引用、但定义在通用层游戏功能 / 前端文件里的符号（清单与处理方案见 coupling.tsv）。
// 每个桩只记下符号名并 abort：B3 一旦走到这里就要响亮地停，绝不静默返回成功。哪条该由 B3 自己实现、哪条该在共享文件里加默认不变的钩子，由架构师按 coupling.tsv 定。
// dm_game_res / front_game_res 已由 B3 自己实现（b3_front.c），这里只剩两个 B3 暂不支持的符号。
// the base runtime coupling stubs: symbols referenced by the reused the base runtime generic sources but defined in the base runtime game-feature / front-end files (inventory and proposals in coupling.tsv).
// Each stub only logs the symbol name and calls abort: if B3 ever gets here it must stop loudly, never return success silently. Which ones B3 implements itself and which need a default-preserving hook in a shared file is decided by the architect from coupling.tsv.
// dm_game_res / front_game_res are implemented by B3 itself (b3_front.c); only the two symbols B3 does not support yet remain here.
#include "dm.h"
#define B3_STUB(sym) do { LOG("FATAL B3 stub: base runtime symbol %s called", sym); abort(); } while (0)

uint64_t svc_host_call(int num, uint64_t *regs) { (void)num; (void)regs; B3_STUB("svc_host_call"); }   //通用层：hle_tact.c（剪口宿主调用 0x80 起）| 通用层: hle_tact.c (cut host calls from 0x80)
void uimsg_subscribe(void (*fn)(const char *, const char *, const char *)) { (void)fn; B3_STUB("uimsg_subscribe"); }   //通用层：diag.c（UI 消息观测门，DM_DETCLOCK 时用）| 通用层: diag.c (UI message observation gate, used under DM_DETCLOCK)
