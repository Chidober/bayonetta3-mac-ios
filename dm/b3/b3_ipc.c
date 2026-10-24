// B3 的最小服务端模拟：B3 的 sdk 直接走服务 IPC（SVC 0x1f ConnectToNamedPort、0x21 SendSyncRequest），不像通用层在 nn:: 层接管。
// 这里解析 HIPC 消息（CMIF 与 TIPC 两种载荷，含 CMIF 域），维护会话句柄表与域内对象表，实现命名端口 "sm:" 和 sm 的取服务；其余按（对象名, 协议, 命令号）查表分派。
// 表里没有的命令：严格模式记 B3-STOP kind=ipc 后 abort；DM_B3_LENIENT=1 时回结果 0、无输出并记 B3-GAP。每个请求都记一行 B3-IPC（dm/b3/tools 的普查工具解析它）。
// 服务行为一律不猜：先记录参数与返回值，再按 switchbrew 公开文档实现最小行为。格式依据：switchbrew「HIPC」「CMIF」「TIPC」「Services API」各页。
// Minimal service-side emulation for B3: B3's sdk talks service IPC directly (SVC 0x1f ConnectToNamedPort, 0x21 SendSyncRequest), unlike the base runtime which takes over at the nn:: layer.
// This parses HIPC messages (CMIF and TIPC payloads, including CMIF domains), keeps a session handle table and per-domain object tables, implements the named port "sm:" and sm's service lookup; the rest is dispatched by (object name, protocol, command id) from a table.
// Commands not in the table: strict mode logs B3-STOP kind=ipc and aborts; with DM_B3_LENIENT=1 it answers result 0 with no output and logs B3-GAP. Every request logs one B3-IPC line (parsed by the census tool in dm/b3/tools).
// No service behaviour is guessed: arguments and return values are recorded first, then the minimal behaviour is implemented from the public switchbrew documentation. Format references: the switchbrew pages "HIPC", "CMIF", "TIPC", "Services API".
#include "dm.h"
#include "b3_ipc.h"

#define B3_SESS_BASE 0x8000u   // 会话句柄 = 0x8000 + 下标，与 hle_thread.c 的 0x1001..0x1fff 不重叠 | session handle = 0x8000 + index, disjoint from hle_thread.c's 0x1001..0x1fff
#define B3_SESS_MAX 256
#define B3_OBJ_MAX 64          // 每个域的对象数上限 | object limit per domain
#define CMIF_IN 0x49434653u    // "SFCI"
#define CMIF_OUT 0x4f434653u   // "SFCO"

typedef struct { int used; char name[40]; } b3_obj_t;
// 域：对象表由转成域的会话和它的克隆共用（Horizon 里克隆域会话得到同一个域）；obj[1] 是原会话自身的对象
// Domain: the object table is shared by the session that was converted and its clones (in Horizon cloning a domain session yields the same domain); obj[1] is the original session's own object
typedef struct { int refs; b3_obj_t obj[B3_OBJ_MAX]; } b3_dom_t;
typedef struct { int used; char name[40]; b3_dom_t *dom; } b3_sess_t;
static b3_sess_t g_sess[B3_SESS_MAX];
static pthread_mutex_t g_ipc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t sess_new(const char *name) {
    for (int i = 1; i < B3_SESS_MAX; i++)
        if (!g_sess[i].used) { memset(&g_sess[i], 0, sizeof g_sess[i]); g_sess[i].used = 1; strlcpy(g_sess[i].name, name, sizeof g_sess[i].name); return B3_SESS_BASE + i; }
    die("B3 IPC 会话表满 | B3 IPC session table full");
}
static void sess_free(b3_sess_t *s) { if (s->dom && --s->dom->refs == 0) free(s->dom); s->dom = NULL; s->used = 0; }
static b3_sess_t *sess_get(uint32_t h) {
    uint32_t i = h - B3_SESS_BASE;
    return h >= B3_SESS_BASE && i < B3_SESS_MAX && g_sess[i].used ? &g_sess[i] : NULL;
}
static uint32_t obj_new(b3_sess_t *s, const char *name) {
    for (uint32_t i = 1; i < B3_OBJ_MAX; i++)
        if (!s->dom->obj[i].used) { s->dom->obj[i].used = 1; strlcpy(s->dom->obj[i].name, name, sizeof s->dom->obj[i].name); return i; }
    die("B3 IPC 域对象表满 | B3 IPC domain object table full");
}
int b3_ipc_is_session(uint32_t h) { return sess_get(h) != NULL; }
b3_buf_t b3_in_buffer(const b3_call_t *c, unsigned i) {
    if (i < 4 && c->h->send[i].size) return c->h->send[i];
    return i < 4 ? c->h->send_static[i] : (b3_buf_t){0, 0};
}
b3_buf_t b3_out_buffer(const b3_call_t *c, unsigned i) {
    if (i < 4 && c->h->recv[i].size) return c->h->recv[i];
    return i < c->h->n_recv_static ? c->h->recv_static[i] : (b3_buf_t){0, 0};
}
uint32_t b3_ipc_open(const char *name, int is_port) { (void)is_port; pthread_mutex_lock(&g_ipc_lock); uint32_t h = sess_new(name); pthread_mutex_unlock(&g_ipc_lock); return h; }

// ---------- HIPC 解析 | HIPC parsing ----------
int b3_hipc_parse(const uint32_t *msg, b3_hipc_t *h) {
    memset(h, 0, sizeof *h);
    h->type = msg[0] & 0xffff;
    h->n_statics = (msg[0] >> 16) & 15; h->n_send = (msg[0] >> 20) & 15; h->n_recv = (msg[0] >> 24) & 15; h->n_exch = (msg[0] >> 28) & 15;
    h->n_data = msg[1] & 0x3ff;
    unsigned w = 2;
    if (msg[1] >> 31) {
        uint32_t sp = msg[w++];
        h->send_pid = sp & 1; h->n_copy = (sp >> 1) & 15; h->n_move = (sp >> 5) & 15;
        if (h->send_pid) w += 2;
        for (unsigned i = 0; i < h->n_copy + h->n_move && i < 8; i++) h->handles[i] = msg[w + i];
        w += h->n_copy + h->n_move;
    }
    // X 描述符（发送静态）各 2 字：打包字（地址 36–38 位在 6–8 位、32–35 位在 12–15 位、大小在 16–31 位），地址低 32 位
    // X descriptors (send statics) are 2 words each: packed word (address bits 36-38 at bits 6-8, bits 32-35 at bits 12-15, size at bits 16-31), address bits 0-31
    for (unsigned k = 0; k < h->n_statics; k++, w += 2) {
        uint32_t p = msg[w];
        if (k < 4) h->send_static[k] = (b3_buf_t){msg[w + 1] | (uint64_t)((p >> 12) & 15) << 32 | (uint64_t)((p >> 6) & 7) << 36, p >> 16};
    }
    // A / B / W 描述符各 3 字：大小低 32 位、地址低 32 位、打包字（地址 36–38 位在 2–4 位、大小 32–35 位在 24–27 位、地址 32–35 位在 28–31 位）
    // A / B / W descriptors are 3 words each: size bits 0-31, address bits 0-31, packed word (address bits 36-38 at bits 2-4, size bits 32-35 at bits 24-27, address bits 32-35 at bits 28-31)
    for (unsigned k = 0; k < h->n_send + h->n_recv + h->n_exch; k++, w += 3) {
        uint32_t p = msg[w + 2];
        b3_buf_t b = {msg[w + 1] | (uint64_t)((p >> 28) & 15) << 32 | (uint64_t)((p >> 2) & 7) << 36, msg[w] | (uint64_t)((p >> 24) & 15) << 32};
        if (k < h->n_send) { if (k < 4) h->send[k] = b; }
        else if (k < h->n_send + h->n_recv) { if (k - h->n_send < 4) h->recv[k - h->n_send] = b; }
    }
    h->data = w;
    if (w + h->n_data > B3_IPC_WORDS) return -1;
    // C 描述符（接收列表）：位置在头字 1 的 20–30 位（0 = 紧跟数据区），个数由接收模式定（2 = 1 个，>2 = 模式 − 2 个）；每项 2 字：地址低 32 位，打包字（地址高 16 位、大小 16 位）
    // C descriptors (receive list): located at word 1 bits 20-30 (0 = right after the data area), count from the receive mode (2 = one, >2 = mode − 2); each entry is 2 words: address bits 0-31, packed word (address bits 32-47, size 16 bits)
    h->recv_mode = (msg[1] >> 10) & 15; h->recv_off = (msg[1] >> 20) & 0x7ff;
    unsigned nc = h->recv_mode == 2 ? 1 : h->recv_mode > 2 ? h->recv_mode - 2 : 0, rw = h->recv_off ? h->recv_off : w + h->n_data;
    for (unsigned k = 0; k < nc && k < 4 && rw + 2 * k + 1 < B3_IPC_WORDS; k++) {
        uint32_t lo = msg[rw + 2 * k], p = msg[rw + 2 * k + 1];
        h->recv_static[k] = (b3_buf_t){lo | (uint64_t)(p & 0xffff) << 32, p >> 16}; h->n_recv_static = k + 1;
    }
    return 0;
}

// ---------- 命令表 | Command table ----------
// 处理函数读 in（原始参数字，已跳过 CMIF / 域头），写 out，返回结果码；要回一个新对象时把对象名写进 obj（域里分对象号，否则开新会话移交句柄）
// A handler reads in (raw argument words, CMIF / domain headers skipped), writes out, returns the result; to return a new object it writes the object name into obj (a domain gets a new object id, otherwise a new session is moved as a handle)

// sm GetServiceHandle（TIPC 1）/ GetService（CMIF 1）：参数是 8 字节服务名，回该服务的会话 | sm GetServiceHandle (TIPC 1) / GetService (CMIF 1): the argument is an 8-byte service name; returns a session for that service
static uint32_t sm_get_service(b3_call_t *c) {
    char name[9] = ""; if (c->n_in >= 2) memcpy(name, c->in, 8);
    if (!name[0]) return 0xc15;   // sm 的 InvalidServiceName（模块 21，描述 6）| sm InvalidServiceName (module 21, description 6)
    strlcpy(c->obj, name, sizeof c->obj); return 0;
}
// sm RegisterClient（TIPC 0）/ Initialize（CMIF 0）：登记客户端进程，无输出 | sm RegisterClient (TIPC 0) / Initialize (CMIF 0): registers the client process, no output
static uint32_t ok_no_output(b3_call_t *c) { (void)c; return 0; }
// 控制请求 CloneCurrentObject（2）/ CloneCurrentObjectEx（4）：回一个指向同一服务的新会话（移交句柄）| Control requests CloneCurrentObject (2) / CloneCurrentObjectEx (4): return a new session to the same service (move handle)
static uint32_t ctl_clone(b3_call_t *c) { c->clone = 1; return 0; }
// 控制请求 QueryPointerBufferSize：回服务端指针缓冲大小（u16）。本模拟直接读客户端内存，指针缓冲是虚设的；取 0x8000（DECISION）
// Control request QueryPointerBufferSize: returns the server's pointer buffer size (u16). This emulation reads client memory directly, so the pointer buffer is nominal; 0x8000 is chosen (DECISION)
static uint32_t ctl_pointer_buffer_size(b3_call_t *c) { c->out[0] = 0x8000; c->n_out = 1; return 0; }

// lm OpenLogger（CMIF 0，参数 u64 pid）：回 ILogger 对象 | lm OpenLogger (CMIF 0, argument u64 pid): returns an ILogger object
static uint32_t lm_open_logger(b3_call_t *c) { strlcpy(c->obj, "lm.ILogger", sizeof c->obj); return 0; }

static const b3_cmd_t g_core_cmds[] = {
    {"sm:", B3_TIPC, 0, ok_no_output, "switchbrew Services API: RegisterClient"},
    {"sm:", B3_TIPC, 1, sm_get_service, "switchbrew Services API: GetServiceHandle"},
    {"sm:", B3_CMIF, 0, ok_no_output, "switchbrew Services API: Initialize"},
    {"sm:", B3_CMIF, 1, sm_get_service, "switchbrew Services API: GetService"},
    {"*", B3_CONTROL, 3, ctl_pointer_buffer_size, "switchbrew CMIF: control QueryPointerBufferSize"},
    {"*", B3_CONTROL, 2, ctl_clone, "switchbrew CMIF: control CloneCurrentObject"},
    {"*", B3_CONTROL, 4, ctl_clone, "switchbrew CMIF: control CloneCurrentObjectEx"},
    {"lm", B3_CMIF, 0, lm_open_logger, "switchbrew Log services: ILogService OpenLogger"},
};

static const char *proto_name(int p) { return p == B3_TIPC ? "tipc" : p == B3_CMIF ? "cmif" : p == B3_CONTROL ? "control" : "other"; }

// 回应：类型 0；可带移交句柄；CMIF 时数据区 16 字节对齐，域时先写域出头，对象号接在输出数据后 | Response: type 0, optional move handles; for CMIF the data area is 16-byte aligned, a domain response starts with the domain out header and the object ids follow the output data
static void respond(uint32_t *msg, int proto, int domain, uint32_t result, const uint32_t *out, unsigned n_out,
                    const uint32_t *handles, unsigned n_handles, const uint32_t *objs, unsigned n_objs,
                    const uint32_t *copies, unsigned n_copies) {
    uint32_t words[B3_IPC_WORDS]; memset(words, 0, sizeof words);
    unsigned w = 2;
    if (n_handles || n_copies) {   // 特殊头：复制句柄在前、移交句柄在后 | special header: copy handles first, then move handles
        words[w++] = (n_copies << 1) | (n_handles << 5);
        for (unsigned i = 0; i < n_copies; i++) words[w++] = copies[i];
        for (unsigned i = 0; i < n_handles; i++) words[w++] = handles[i];
    }
    unsigned start = w, d = w;
    if (proto == B3_TIPC) words[d++] = result;
    else {
        d = (w + 3) & ~3u;
        if (domain) { words[d] = n_objs; d += 4; }
        words[d] = CMIF_OUT; words[d + 1] = 0; words[d + 2] = result; words[d + 3] = 0; d += 4;
    }
    for (unsigned i = 0; i < n_out; i++) words[d++] = out[i];
    for (unsigned i = 0; i < n_objs; i++) words[d++] = objs[i];
    unsigned n_data = d - start + (proto == B3_TIPC ? 0 : 4);   // CMIF 习惯在末尾多留 16 字节 | CMIF customarily reserves 16 extra bytes at the end
    words[0] = 0; words[1] = n_data | (n_handles || n_copies ? 1u << 31 : 0);
    memcpy(msg, words, sizeof words);
}

// 处理会话 sh 上 msg 里的一条请求并写回应答；返回 0 = 已应答，1 = 会话已关（无应答），-1 = 严格模式停止点 | Handle one request in msg on session sh and write the answer back; returns 0 = answered, 1 = session closed (no answer), -1 = strict-mode stop
int b3_ipc_handle(uint32_t sh, uint32_t *msg, int lenient) {
    b3_sess_t *s = sess_get(sh);
    if (!s) return -2;
    b3_hipc_t h;
    if (b3_hipc_parse(msg, &h)) { LOG("B3-IPC service=%s malformed header %#x %#x", s->name, msg[0], msg[1]); return -1; }
    if (h.type == 2) { LOG("B3-IPC service=%s proto=close cmd=0 handled=1 result=0", s->name); b3_object_closed(s->name); sess_free(s); return 1; }
    int proto; uint32_t cmd, obj_id = 0; const char *oname = s->name; b3_call_t c; memset(&c, 0, sizeof c); c.h = &h;
    if (h.type >= 16) { proto = B3_TIPC; cmd = h.type - 16; c.in = msg + h.data; c.n_in = h.n_data; }
    else if (h.type == 4 || h.type == 5 || h.type == 6 || h.type == 7) {
        proto = (h.type == 5 || h.type == 7) ? B3_CONTROL : B3_CMIF;
        unsigned d = (h.data + 3) & ~3u, end = h.data + h.n_data;
        if (proto == B3_CMIF && s->dom) {   // 域入头：类型 u8、入对象数 u8、数据长 u16、对象号 u32、填充、令牌 | domain in header: type u8, in-object count u8, data size u16, object id u32, padding, token
            unsigned dtype = msg[d] & 0xff; obj_id = msg[d + 1];
            if (obj_id >= B3_OBJ_MAX || !s->dom->obj[obj_id].used) { LOG("B3-IPC service=%s bad domain object %u", s->name, obj_id); return -1; }
            oname = s->dom->obj[obj_id].name;
            if (dtype == 2) {   // 关闭域内对象 | close a domain object
                LOG("B3-IPC service=%s proto=close cmd=0 handled=1 result=0", oname); b3_object_closed(oname); s->dom->obj[obj_id].used = 0;
                respond(msg, B3_CMIF, 1, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0); return 0;
            }
            unsigned n_in_obj = (msg[d] >> 8) & 0xff, data_size = msg[d] >> 16;   // 入对象号接在数据载荷后 | in-object ids follow the data payload
            for (unsigned k = 0; k < n_in_obj && k < 4; k++) {
                uint32_t id = msg[d + 4 + data_size / 4 + k];
                if (id < B3_OBJ_MAX && s->dom->obj[id].used) strlcpy(c.in_obj[c.n_in_obj++], s->dom->obj[id].name, sizeof c.in_obj[0]);
            }
            d += 4;
        }
        if (d + 4 > end || msg[d] != CMIF_IN) { LOG("B3-IPC service=%s bad CMIF header", oname); return -1; }
        cmd = msg[d + 2]; c.in = msg + d + 4; c.n_in = end - (d + 4);
    } else { proto = -1; cmd = h.type; c.in = msg + h.data; c.n_in = h.n_data; }
    int domain = proto == B3_CMIF && s->dom;
    c.self = oname;
    char base[40]; strlcpy(base, oname, sizeof base); char *inst = strchr(base, '#'); if (inst) *inst = 0;   // 按基名查表（"am.IStorage#3" → "am.IStorage"；服务名里的 ":" 不动）| look up by base name ("am.IStorage#3" → "am.IStorage"; a ":" in service names is untouched)
    char args[96] = ""; for (unsigned i = 0, p = 0; i < c.n_in && i < 8 && p < sizeof args - 10; i++) p += snprintf(args + p, sizeof args - p, "%s%08x", i ? " " : "", c.in[i]);

    // 控制请求 ConvertCurrentObjectToDomain：会话转成域，会话本身成对象 1，回 u32 对象号 | Control request ConvertCurrentObjectToDomain: the session becomes a domain, the session itself becomes object 1; returns the u32 object id
    if (proto == B3_CONTROL && cmd == 0) {
        if (!s->dom) { s->dom = calloc(1, sizeof *s->dom); s->dom->refs = 1; obj_new(s, s->name); }
        uint32_t one = 1;
        LOG("B3-IPC service=%s proto=control cmd=0 handled=1 result=0 words=%u args=[%s]", s->name, c.n_in, args);
        respond(msg, B3_CONTROL, 0, 0, &one, 1, NULL, 0, NULL, 0, NULL, 0); return 0;
    }
    const b3_cmd_t *e = NULL;
    const b3_cmd_t *tabs[6] = {g_core_cmds, b3_applet_cmds, b3_libapplet_cmds, b3_graphics_cmds, b3_audio_cmds, b3_save_cmds};
    unsigned ns[6] = {sizeof g_core_cmds / sizeof *g_core_cmds, b3_applet_ncmds, b3_libapplet_ncmds, b3_graphics_ncmds, b3_audio_ncmds, b3_save_ncmds};
    for (unsigned t = 0; t < 6 && !e; t++)
        for (unsigned i = 0; i < ns[t]; i++)
            if ((!strcmp(tabs[t][i].object, base) || (proto == B3_CONTROL && !strcmp(tabs[t][i].object, "*"))) && tabs[t][i].proto == proto && tabs[t][i].cmd == cmd) { e = &tabs[t][i]; break; }
    if (!e) {
        extern uint64_t b3_svc_calls;
        LOG("B3-IPC service=%s proto=%s cmd=%u handled=0 result=none words=%u args=[%s]", oname, proto_name(proto), cmd, c.n_in, args);
        LOG("%s kind=ipc service=%s proto=%s cmd=%u svc_calls=%llu", lenient ? "B3-GAP" : "B3-STOP", oname, proto_name(proto), cmd, b3_svc_calls);
        if (!lenient) return -1;
        respond(msg, proto, domain, 0, NULL, 0, NULL, 0, NULL, 0, NULL, 0); return 0;
    }
    uint32_t r = e->fn(&c), hnd = 0, oid = 0;
    if (!r && c.clone) {   // 克隆：同一服务的新会话；原会话是域时共用同一个域 | clone: a new session to the same service; shares the domain if the original is one
        hnd = sess_new(s->name); b3_sess_t *n = sess_get(hnd); if (s->dom) { n->dom = s->dom; s->dom->refs++; }
        LOG("B3-IPC-SESSION service=%s handle=%#x", s->name, hnd); }
    if (!r && c.obj[0]) {
        if (domain) oid = obj_new(s, c.obj);
        else { hnd = sess_new(c.obj); LOG("B3-IPC-SESSION service=%s handle=%#x", c.obj, hnd); }
    }
    // 同一（对象、命令）只完整记前 32 次，之后每 4096 次记一次并带累计数（音频线程每毫秒轮询会把日志撑到 GB 级，实测）
    // Log a given (object, command) in full only the first 32 times, then once every 4096 with the running count (the audio thread's per-millisecond polling grew the log to GBs, measured in )
    { static struct { uint32_t hash, n; } seen[1024]; uint32_t hh = cmd * 2654435761u; for (const char *q = base; *q; q++) hh = hh * 31 + (uint8_t)*q;
      unsigned slot = hh % 1024; if (seen[slot].hash != hh) { seen[slot].hash = hh; seen[slot].n = 0; }
      uint32_t k = ++seen[slot].n;
      if (k <= 32 || k % 4096 == 0)
          LOG("B3-IPC service=%s proto=%s cmd=%u handled=1 result=%#x words=%u args=[%s]%s%s%s", oname, proto_name(proto), cmd, r, c.n_in, args, c.obj[0] ? " object=" : "", c.obj, k > 32 ? " (repeated)" : ""); }
    respond(msg, proto, domain, r, c.out, c.n_out, hnd ? &hnd : NULL, hnd ? 1 : 0, oid ? &oid : NULL, oid ? 1 : 0, c.copy, c.n_copy);
    return 0;
}

// ---------- SVC 入口（hle_svc.c 在 DM_B3 下先调这里）| SVC entry (hle_svc.c calls this first under DM_B3) ----------
// 返回 1 = 已处理（结果在 regs[0]），0 = 不是 IPC 相关，交回原分派 | Returns 1 = handled (result in regs[0]), 0 = not IPC related, back to the normal dispatch
int b3_ipc_svc(int num, uint64_t *regs) {
    const char *e = getenv("DM_B3_LENIENT"); int lenient = e && !strcmp(e, "1");
    switch (num) {
    case 0x1f: {   // ConnectToNamedPort(x1 = 端口名) → w0 结果、w1 会话句柄 | ConnectToNamedPort(x1 = port name) → w0 result, w1 session handle
        char name[16] = ""; strlcpy(name, (const char *)regs[1], sizeof name);
        if (strcmp(name, "sm:")) { LOG("B3-IPC port=%s unknown named port", name); return 0; }   // 未知端口交回严格模式的未知 SVC 处理 | unknown ports fall back to strict-mode unknown SVC handling
        uint32_t h = b3_ipc_open(name, 1);
        LOG("B3-IPC-SESSION service=%s handle=%#x", name, h);
        regs[0] = 0; regs[1] = h; return 1;
    }
    case 0x21: {   // SendSyncRequest(w0 = 会话句柄)：消息在本线程的 TLS 开头 | SendSyncRequest(w0 = session handle): the message is at the start of this thread's TLS
        uint32_t *msg = pthread_getspecific(g_keytlr);
        pthread_mutex_lock(&g_ipc_lock);
        if (!sess_get((uint32_t)regs[0])) { pthread_mutex_unlock(&g_ipc_lock); return 0; }
        int r = b3_ipc_handle((uint32_t)regs[0], msg, lenient);
        pthread_mutex_unlock(&g_ipc_lock);
        if (r == -1 && !lenient) abort();
        regs[0] = r == 1 ? 0xf601 : 0; return 1;   // 关闭后再发：会话已关（kernel 0xf601）| sending after close: session closed (kernel 0xf601)
    }
    case 0x15: {   // CreateTransferMemory(x1 地址, x2 大小, w3 权限) → w0 结果、w1 句柄 | CreateTransferMemory(x1 address, x2 size, w3 permission) → w0 result, w1 handle
        uint32_t b3_tmem_new(uint64_t, uint64_t);
        regs[1] = b3_tmem_new(regs[1], regs[2]); regs[0] = 0; return 1;
    }
    case 0x16: {   // CloseHandle：只接管会话句柄 | CloseHandle: only session handles are taken over
        pthread_mutex_lock(&g_ipc_lock);
        b3_sess_t *s = sess_get((uint32_t)regs[0]);
        if (s) { LOG("B3-IPC service=%s proto=close cmd=0 handled=1 result=0", s->name); b3_object_closed(s->name); sess_free(s); }
        pthread_mutex_unlock(&g_ipc_lock);
        int b3_shmem_close(uint32_t);
        int b3_tmem_close(uint32_t);
        if (!s && (b3_shmem_close((uint32_t)regs[0]) || b3_tmem_close((uint32_t)regs[0]))) { regs[0] = 0; return 1; }   // b3_kernel.c 的共享内存 / 传输内存句柄 | shared / transfer memory handles of b3_kernel.c
        if (!s) return 0;
        regs[0] = 0; return 1;
    }
    }
    return 0;
}
