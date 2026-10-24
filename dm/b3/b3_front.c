// B3 对通用层前端接口的实现：thread.c 的 guest_thread_init 在每个客体线程初始化时调 dm_game_res / front_game_res，把结果写进分辨率参数位。
// 这两个参数位只在改写器 PARAMS 规则引用时才被客体读；B3 没有 PARAMS 规则，所以这里固定返回 0 = “无覆盖”：{宽 0, 高 0}，thread.c 见高为 0 时也不改宽高比上限。
// 以后 B3 有自己的前端和分辨率规则时，在这里换成真实值。
// B3's implementation of the base runtime front-end interface: guest_thread_init in thread.c calls dm_game_res / front_game_res on every guest thread init and writes the result into the resolution parameter slots.
// The guest only reads these slots when a rewriter PARAMS rule references them; B3 has no PARAMS rules, so this returns a fixed 0 = "no override": {width 0, height 0}, and with height 0 thread.c leaves the aspect ratio limit alone too.
// When B3 gets its own front end and resolution rules, replace this with the real values .
#include "dm.h"
#include "front.h"
#include "dm_uiscreen.h"

uint64_t dm_game_res(int docked) { (void)docked; return 0; }      // 无覆盖 | no override
uint64_t front_game_res(int docked) { (void)docked; return 0; }   // 无覆盖 | no override

// 无窗口的缺省实现（弱符号）：单测程序只链这个文件；b3host 另链 b3_front_mac.m，那里的强定义在 B3_WINDOW=1 时开窗口
// Windowless defaults (weak symbols): test programs link only this file; b3host also links b3_front_mac.m, whose strong definitions open a window when B3_WINDOW=1 
__attribute__((weak)) void *front_metal_layer(void) { return NULL; }   // 出图的 CAMetalLayer；NULL = 无窗口，Metal 层照常建资源、不出图 | the CAMetalLayer to present to; NULL = no window, the Metal layer still builds resources but presents nothing
__attribute__((weak)) void b3_front_run(void (*guest)(void)) { guest(); }   // 客体在当前线程跑 | the guest runs on the current thread
