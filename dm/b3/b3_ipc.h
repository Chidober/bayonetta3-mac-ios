// B3 最小服务端模拟的接口（实现见 b3_ipc.c）；单测程序 tests/ipc_harness.c 直接调 b3_ipc_handle。
// Interface of B3's minimal service-side emulation (implementation in b3_ipc.c); the test program tests/ipc_harness.c calls b3_ipc_handle directly.
#pragma once
#include <stdint.h>
#define B3_IPC_WORDS 64        // TLS 开头 0x100 字节的消息区 | the 0x100-byte message area at the start of TLS
enum { B3_CMIF, B3_TIPC, B3_CONTROL };
typedef struct { uint64_t addr, size; } b3_buf_t;
typedef struct {
    unsigned type, n_statics, n_send, n_recv, n_exch, n_data, send_pid, n_copy, n_move, data, recv_mode, recv_off;
    uint32_t handles[8];
    b3_buf_t send[4], recv[4], recv_static[4], send_static[4];   // A、B、C、X 描述符（各取前 4 个）| A, B, C, X descriptors (first 4 of each)
    unsigned n_recv_static;
} b3_hipc_t;
int b3_hipc_parse(const uint32_t *msg, b3_hipc_t *h);
int b3_ipc_handle(uint32_t session, uint32_t *msg, int lenient);
int b3_ipc_svc(int num, uint64_t *regs);
int b3_ipc_is_session(uint32_t h);
uint32_t b3_ipc_open(const char *name, int is_port);   // 单测用：直接开一个会话 | for tests: open a session directly

// 命令表项：处理函数读 in（原始参数字，已跳过 CMIF / 域头），写 out，返回结果码；要回新对象时写对象名到 obj；要回复制句柄时填 copy
// Command table entry: a handler reads in (raw argument words, CMIF / domain headers skipped), writes out, returns the result; writes an object name into obj to return a new object; fills copy to return copy handles
typedef struct { const uint32_t *in; unsigned n_in; uint32_t out[32]; unsigned n_out; char obj[40]; uint32_t copy[4]; unsigned n_copy;
                 const b3_hipc_t *h; uint32_t clone;
                 const char *self;               // 被调用对象的全名（可带 "#实例号"）| full name of the called object (may carry "#instance")
                 char in_obj[4][40]; unsigned n_in_obj;   // 随请求传入的对象（域入对象）| objects passed in with the request (domain in-objects)
               } b3_call_t;   // h：本请求的描述符；clone 非 0：回一个指向同一服务的新会话 | h: this request's descriptors; clone non-zero: return a new session to the same service
b3_buf_t b3_out_buffer(const b3_call_t *c, unsigned i);
b3_buf_t b3_in_buffer(const b3_call_t *c, unsigned i);    // 第 i 个输入缓冲：A 有大小就用它，否则用第 i 个 X | the i-th input buffer: the A descriptor if it has a size, else the i-th X   // 第 i 个输出缓冲：B 描述符有大小就用它，否则用第 i 个 C | the i-th output buffer: the B descriptor if it has a size, else the i-th C
typedef uint32_t (*b3_cmd_fn)(b3_call_t *c);
// 宿主唯一的用户账号 Uid（DECISION：固定非零值，PopLaunchParameter 预选用户与 acc 服务共用）| The host's only user account Uid (DECISION: a fixed non-zero value shared by the PopLaunchParameter preselected user and the acc service)
#define B3_USER_UID_LO 0x0000000000000001ull
#define B3_USER_UID_HI 0x00000000000000b3ull
typedef struct { const char *object; int proto; uint32_t cmd; b3_cmd_fn fn; const char *doc; } b3_cmd_t;
extern const b3_cmd_t b3_applet_cmds[];   // b3_applet.c | b3_applet.c
extern const unsigned b3_applet_ncmds;
extern const b3_cmd_t b3_libapplet_cmds[];   // b3_libapplet.c | b3_libapplet.c
extern const unsigned b3_libapplet_ncmds;
extern const b3_cmd_t b3_graphics_cmds[];   // b3_graphics.c | b3_graphics.c
extern const unsigned b3_graphics_ncmds;
extern const b3_cmd_t b3_audio_cmds[];   // b3_audio.c | b3_audio.c
extern const unsigned b3_audio_ncmds;
extern const b3_cmd_t b3_save_cmds[];   // b3_save.c | b3_save.c
extern const unsigned b3_save_ncmds;
void b3_object_closed(const char *name);   // 会话 / 域对象关闭时调用，释放实例资源（b3_save.c）| called when a session / domain object closes, frees instance resources (b3_save.c)
