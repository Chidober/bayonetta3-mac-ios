// B3 的音频服务（audren:u）：最小静音实现。命令号与参数按 switchbrew「Audio services」页；RequestUpdate 的输入 / 输出布局按 libnx audren.h（ISC，公开接口说明，只取布局不拷代码），
// 输出表示“全部消费、全部静音”：内存池按请求转到已附着 / 已分离，语音报 0 播放样本，各段大小按输入的对象数算。游戏要求修订号 REV10，libnx 只写到 REV6，新增字段以游戏实际检查为准。
// 帧事件：Start 之后由宿主线程每 5 ms 触发一次（libnx AUDREN_TIMER_PERIOD_MS），Stop 后停。audren 不出声；游戏实际用 audout 出声，audout 的样本交给 b3_sound_mac.c。
// B3 audio service (audren:u): minimal silent implementation. Command ids and arguments follow the switchbrew page "Audio services"; the RequestUpdate input / output layout follows libnx audren.h (ISC, public interface notes; layout only, no code copied).
// The output says "everything consumed, all silent": memory pools move to attached / detached as requested, voices report 0 played samples, and section sizes come from the input object counts. The game asks for revision REV10 while libnx stops at REV6; newer fields follow what the game actually checks .
// Frame event: after Start a host thread signals it every 5 ms (libnx AUDREN_TIMER_PERIOD_MS) until Stop. audren produces no sound; the game actually plays through audout, whose samples go to b3_sound_mac.c .
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "dm.h"
#include "b3_ipc.h"

int svc_thread(int num, uint64_t *regs);   // hle_thread.c

// AudioRendererParameter（0x34 字节，libnx audren.c）| AudioRendererParameter (0x34 bytes, libnx audren.c)
typedef struct {
    int32_t sample_rate, sample_count, mix_buffer_count, submix_count, voice_count, sink_count, effect_count, perf_frames;
    uint8_t voice_drop, pad[3];
    int32_t splitter_count, splitter_send_count, behavior, revision;
} audren_param_t;
_Static_assert(sizeof(audren_param_t) == 0x34, "AudioRendererParameter 0x34");

#define MAX_RENDERERS 4
static struct { int used, started; audren_param_t p; uint32_t ev_read, ev_write; uint64_t updates; } g_r[MAX_RENDERERS];
static int renderer_index(const b3_call_t *c) {   // 对象名 "audren.IAudioRenderer#k" 的 k | k in the object name "audren.IAudioRenderer#k"
    const char *h = c->self ? strchr(c->self, '#') : NULL; int k = h ? atoi(h + 1) : -1;
    if (k < 0 || k >= MAX_RENDERERS || !g_r[k].used) die("B3 audren：未知渲染器实例 %s | unknown renderer instance", c->self ? c->self : "(null)");
    return k;
}

// 帧事件线程：对所有已启动的渲染器每 5 ms 触发一次 | Frame event thread: signals every started renderer every 5 ms
static void *frame_thread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(5000);
        for (int k = 0; k < MAX_RENDERERS; k++)
            if (g_r[k].used && g_r[k].started) { uint64_t r[8] = {g_r[k].ev_write}; svc_thread(0x11, r); }
    }
    return NULL;
}
static void frame_thread_start(void) {
    static atomic_int once; if (atomic_exchange(&once, 1)) return;
    pthread_t t; if (pthread_create(&t, NULL, frame_thread, NULL)) die("B3 audren 帧线程创建失败 | frame thread creation failed");
    pthread_detach(t);
}

// ---------- IAudioRendererManager ----------
// 1 GetWorkBufferSize(AudioRendererParameter) → u64：服务端不使用这块内存，给一个随规模增长的页对齐大小（DECISION）
// 1 GetWorkBufferSize(AudioRendererParameter) → u64: the server does not use this memory; return a page-aligned size that grows with the configuration (DECISION)
static uint32_t mgr_work_buffer_size(b3_call_t *c) {
    if (c->n_in * 4 < sizeof(audren_param_t)) return 0x2c99;   // 参数不全：audio 模块 153 的 InvalidArgument 一类 | incomplete parameter: audio module 153 InvalidArgument-like
    audren_param_t p; memcpy(&p, c->in, sizeof p);
    uint64_t n = 0x10000 + (uint64_t)p.mix_buffer_count * (p.sample_count + 0x10) * 4 + (uint64_t)p.voice_count * 0x400 +
                 (uint64_t)p.effect_count * 0x800 + (uint64_t)p.sink_count * 0x200 + (uint64_t)(p.submix_count + 1) * 0x200;
    n = (n + 0xFFF) & ~0xFFFull;
    LOG("B3-AUDREN work_buffer rate=%d count=%d mixbuf=%d submix=%d voices=%d sinks=%d effects=%d splitters=%d rev=%#x → %#llx",
        p.sample_rate, p.sample_count, p.mix_buffer_count, p.submix_count, p.voice_count, p.sink_count, p.effect_count, p.splitter_count, p.revision, n);
    memcpy(c->out, &n, 8); c->n_out = 2; return 0;
}
// 0 OpenAudioRenderer(AudioRendererParameter, u64 工作区大小, u64 aruid, 进程号, 句柄：工作区传输内存、进程) → IAudioRenderer
// 0 OpenAudioRenderer(AudioRendererParameter, u64 work buffer size, u64 aruid, pid, handles: work buffer transfer memory, process) → IAudioRenderer
static uint32_t mgr_open_renderer(b3_call_t *c) {
    if (c->n_in * 4 < sizeof(audren_param_t)) return 0x2c99;
    int k = 0; while (k < MAX_RENDERERS && g_r[k].used) k++;
    if (k == MAX_RENDERERS) die("B3 audren：渲染器实例用尽 | out of renderer instances");
    memset(&g_r[k], 0, sizeof g_r[k]); memcpy(&g_r[k].p, c->in, sizeof g_r[k].p); g_r[k].used = 1;
    uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 audren：建事件失败 | event creation failed");
    g_r[k].ev_write = (uint32_t)r[1]; g_r[k].ev_read = (uint32_t)r[2];
    snprintf(c->obj, sizeof c->obj, "audren.IAudioRenderer#%d", k);
    return 0;
}
static uint32_t mgr_audio_device(b3_call_t *c) { strlcpy(c->obj, "audren.IAudioDevice", sizeof c->obj); return 0; }   // 2 GetAudioDeviceService / 4 …WithRevisionInfo

// ---------- IAudioRenderer ----------
static uint32_t r_sample_rate(b3_call_t *c) { c->out[0] = (uint32_t)g_r[renderer_index(c)].p.sample_rate; c->n_out = 1; return 0; }
static uint32_t r_sample_count(b3_call_t *c) { c->out[0] = (uint32_t)g_r[renderer_index(c)].p.sample_count; c->n_out = 1; return 0; }
static uint32_t r_mix_buffer_count(b3_call_t *c) { c->out[0] = (uint32_t)g_r[renderer_index(c)].p.mix_buffer_count; c->n_out = 1; return 0; }
static uint32_t r_state(b3_call_t *c) { c->out[0] = g_r[renderer_index(c)].started ? 0 : 1; c->n_out = 1; return 0; }   // 0 = 已启动，1 = 已停止 | 0 = started, 1 = stopped
static uint32_t r_start(b3_call_t *c) { g_r[renderer_index(c)].started = 1; frame_thread_start(); return 0; }
static uint32_t r_stop(b3_call_t *c) { g_r[renderer_index(c)].started = 0; return 0; }
static uint32_t r_system_event(b3_call_t *c) { c->copy[0] = g_r[renderer_index(c)].ev_read; c->n_copy = 1; return 0; }
static uint32_t r_set_time_limit(b3_call_t *c) { (void)renderer_index(c); return 0; }
static uint32_t r_get_time_limit(b3_call_t *c) { (void)renderer_index(c); c->out[0] = 100; c->n_out = 1; return 0; }

// RequestUpdate 头（0x40 字节，libnx AudioRendererUpdateDataHeader）的字下标 | Word indices of the RequestUpdate header (0x40 bytes, libnx AudioRendererUpdateDataHeader)
enum { H_REV, H_BEHAVIOR, H_MEMPOOLS, H_VOICES, H_CHANNELS, H_EFFECTS, H_MIXES, H_SINKS, H_PERF, H_TOTAL = 15 };
// 4 RequestUpdate / 10 RequestUpdateAuto：输出缓冲 0 = 更新结果，1 = 性能缓冲；输入缓冲 0 = 更新参数
// 4 RequestUpdate / 10 RequestUpdateAuto: output buffer 0 = update result, 1 = performance buffer; input buffer 0 = update parameters
static uint32_t r_request_update(b3_call_t *c) {
    int k = renderer_index(c);
    b3_buf_t in = b3_in_buffer(c, 0), out = b3_out_buffer(c, 0);
    if (in.size < 0x40 || out.size < 0x40) die("B3 audren RequestUpdate 缓冲太小 in=%#llx out=%#llx | buffers too small", in.size, out.size);
    const uint32_t *ih = (const uint32_t *)(uintptr_t)in.addr;
    if (!g_r[k].updates++)
        LOG("B3-AUDREN update_in rev=%#x behavior=%#x mempools=%#x voices=%#x channels=%#x effects=%#x mixes=%#x sinks=%#x perf=%#x w9..14=%#x %#x %#x %#x %#x %#x total=%#x in=%#llx out=%#llx",
            ih[0], ih[1], ih[2], ih[3], ih[4], ih[5], ih[6], ih[7], ih[8], ih[9], ih[10], ih[11], ih[12], ih[13], ih[14], ih[15], in.size, out.size);
    // 输入段按头里的大小依次排在头后：行为、内存池、通道、语音、效果、（分离器）、混音、汇、性能（libnx 注释的顺序）
    // Input sections follow the header in the order of the sizes it lists: behavior, memory pools, channels, voices, effects, (splitters), mixes, sinks, performance (the order in the libnx notes)
    const uint8_t *mp_in = (const uint8_t *)ih + 0x40 + ih[H_BEHAVIOR];
    unsigned n_pools = ih[H_MEMPOOLS] / 0x20, n_voices = ih[H_VOICES] / 0x170, n_effects = ih[H_EFFECTS] / 0xC0, n_sinks = ih[H_SINKS] / 0x140;
    uint32_t sz_pools = n_pools * 0x10, sz_voices = n_voices * 0x10, sz_effects = n_effects * 0x10, sz_sinks = n_sinks * 0x20, sz_perf = 0x10, sz_behavior = 0xB0;
    uint32_t total = 0x40 + sz_pools + sz_voices + sz_effects + sz_sinks + sz_perf + sz_behavior;
    if (out.size < total) die("B3 audren RequestUpdate 输出缓冲 %#llx 小于 %#x | output buffer smaller than needed", out.size, total);
    uint8_t *o = (uint8_t *)(uintptr_t)out.addr; memset(o, 0, total);
    uint32_t *oh = (uint32_t *)o;
    oh[H_REV] = ih[H_REV]; oh[H_MEMPOOLS] = sz_pools; oh[H_VOICES] = sz_voices; oh[H_EFFECTS] = sz_effects; oh[H_SINKS] = sz_sinks;
    oh[H_PERF] = sz_perf; oh[H_BEHAVIOR] = sz_behavior; oh[H_TOTAL] = total;
    // 内存池：请求附着（4）→ 已附着（5），请求分离（2）→ 已分离（3），其余不变 | Memory pools: RequestAttach (4) → Attached (5), RequestDetach (2) → Detached (3), others unchanged
    for (unsigned i = 0; i < n_pools; i++) {
        uint32_t st; memcpy(&st, mp_in + i * 0x20 + 0x10, 4);
        uint32_t ns = st == 4 ? 5 : st == 2 ? 3 : st;
        memcpy(o + 0x40 + i * 0x10, &ns, 4);
    }
    b3_buf_t perf = b3_out_buffer(c, 1); if (perf.size) memset((void *)(uintptr_t)perf.addr, 0, perf.size);
    return 0;
}

// ---------- IAudioDevice ----------
// 设备名表只有一项 "MainAudioOut"（libnx AUDREN_DEFAULT_DEVICE_NAME）；DeviceName 每项 0x100 字节
// The device name list has one entry, "MainAudioOut" (libnx AUDREN_DEFAULT_DEVICE_NAME); each DeviceName is 0x100 bytes
static uint32_t dev_list_names(b3_call_t *c) {
    b3_buf_t b = b3_out_buffer(c, 0); int n = 0;
    if (b.size >= 0x100) { memset((void *)(uintptr_t)b.addr, 0, 0x100); strlcpy((char *)(uintptr_t)b.addr, "MainAudioOut", 0x100); n = 1; }
    c->out[0] = (uint32_t)n; c->n_out = 1; return 0;
}
static uint32_t dev_active_name(b3_call_t *c) {
    b3_buf_t b = b3_out_buffer(c, 0);
    if (b.size) { memset((void *)(uintptr_t)b.addr, 0, b.size); strlcpy((char *)(uintptr_t)b.addr, "MainAudioOut", b.size); }
    return 0;
}
static uint32_t dev_set_volume(b3_call_t *c) { (void)c; return 0; }
static uint32_t dev_get_volume(b3_call_t *c) { float v = 1.0f; memcpy(c->out, &v, 4); c->n_out = 1; return 0; }
static uint32_t dev_new_event(b3_call_t *c) {   // 设备变化事件：没有变化，不触发 | device change events: no change happens, never signaled
    uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 audren：建事件失败 | event creation failed");
    c->copy[0] = (uint32_t)r[2]; c->n_copy = 1; return 0;
}
static uint32_t dev_channel_count(b3_call_t *c) { c->out[0] = 2; c->n_out = 1; return 0; }   // 立体声（DECISION）| stereo (DECISION)

// ---------- audout:u（switchbrew「Audio services」audout 节）：静音播放，缓冲按自身时长到点释放并触发缓冲事件 ----------
// ---------- audout:u (switchbrew "Audio services" audout section): silent playback; each buffer is released after its own duration and the buffer event is signaled ----------
#define MAX_OUTS 8
#define OUT_QUEUE 64
typedef struct { uint64_t client, samples; } out_buf_t;
static struct {
    int used, started; int32_t rate, channels; uint32_t ev_read, ev_write;
    out_buf_t q[OUT_QUEUE]; int qh, qn;            // 待播队列（环形）| pending queue (ring)
    uint64_t rel[OUT_QUEUE]; int rn;               // 已释放、未取走的客户端指针 | released client pointers not yet fetched
    uint64_t played, due_ns;                       // 已播样本、队首缓冲的到点时刻 | samples played, due time of the head buffer
} g_out[MAX_OUTS];
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void *out_thread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(1000);
        pthread_mutex_lock(&g_out_lock);
        uint64_t t = mono_ns();
        for (int k = 0; k < MAX_OUTS; k++) {
            int signal = 0;
            // 按时长释放（不能按“已经听到”释放：游戏只排几十毫秒的缓冲，等设备延迟（蓝牙可达 170 ms）再放会让它断粮、出声断续）
            // Release by duration (not by "heard": the game queues only a few tens of ms, so waiting for the device latency (up to 170 ms on Bluetooth) would starve it and make the sound stutter)
            while (g_out[k].used && g_out[k].started && g_out[k].qn && t >= g_out[k].due_ns && g_out[k].rn < OUT_QUEUE) {
                out_buf_t b = g_out[k].q[g_out[k].qh]; g_out[k].qh = (g_out[k].qh + 1) % OUT_QUEUE; g_out[k].qn--;
                g_out[k].rel[g_out[k].rn++] = b.client; g_out[k].played += b.samples; signal = 1;
                if (g_out[k].qn) g_out[k].due_ns += g_out[k].q[g_out[k].qh].samples * 1000000000ull / (uint64_t)g_out[k].rate;
            }
            if (signal) { uint64_t r[8] = {g_out[k].ev_write}; svc_thread(0x11, r); }
        }
        pthread_mutex_unlock(&g_out_lock);
    }
    return NULL;
}
static void out_thread_start(void) {
    static atomic_int once; if (atomic_exchange(&once, 1)) return;
    pthread_t t; if (pthread_create(&t, NULL, out_thread, NULL)) die("B3 audout 线程创建失败 | thread creation failed");
    pthread_detach(t);
}
static void put_name(b3_buf_t b, const char *name) { if (b.size) { memset((void *)(uintptr_t)b.addr, 0, b.size); strlcpy((char *)(uintptr_t)b.addr, name, b.size); } }
// 0 / 2 ListAudioOuts：一个设备 "DeviceOut"（AudioOutInfo 每项 0x100 字节名字）| 0 / 2 ListAudioOuts: one device, "DeviceOut" (AudioOutInfo is a 0x100-byte name per entry)
static uint32_t ao_list(b3_call_t *c) { b3_buf_t b = b3_out_buffer(c, 0); int n = b.size >= 0x100; if (n) put_name(b, "DeviceOut"); c->out[0] = n; c->n_out = 1; return 0; }
// 1 / 3 OpenAudioOut(AudioOutParameter {u32 采样率, u16 声道, u16 保留}, u64 aruid, 进程句柄, 名字入 / 出缓冲) → IAudioOut, AudioOutParameterInternal {采样率, 声道, 格式 2 = PCM16, 状态 1 = 已停止}；0 表示取默认值 48000 / 2
// 1 / 3 OpenAudioOut(AudioOutParameter {u32 rate, u16 channels, u16 reserved}, u64 aruid, process handle, name in / out buffers) → IAudioOut, AudioOutParameterInternal {rate, channels, format 2 = PCM16, state 1 = stopped}; 0 means the defaults 48000 / 2
static uint32_t ao_open(b3_call_t *c) {
    int32_t rate = c->n_in >= 1 && c->in[0] ? (int32_t)c->in[0] : 48000, ch = c->n_in >= 2 && (c->in[1] & 0xFFFF) ? (int32_t)(c->in[1] & 0xFFFF) : 2;
    if (rate != 48000) return 0x2c99;   // 硬件只有 48 kHz（DECISION：按文档的默认值收，其他拒） | the hardware runs at 48 kHz only (DECISION: accept the documented default, refuse others)
    pthread_mutex_lock(&g_out_lock);
    int k = 0; while (k < MAX_OUTS && g_out[k].used) k++;
    if (k == MAX_OUTS) die("B3 audout：输出实例用尽 | out of audio out instances");
    memset(&g_out[k], 0, sizeof g_out[k]); g_out[k].used = 1; g_out[k].rate = rate; g_out[k].channels = ch;
    pthread_mutex_unlock(&g_out_lock);
    uint64_t r[8] = {0}; if (!svc_thread(0x45, r) || r[0]) die("B3 audout：建事件失败 | event creation failed");
    g_out[k].ev_write = (uint32_t)r[1]; g_out[k].ev_read = (uint32_t)r[2];
    put_name(b3_out_buffer(c, 0), "DeviceOut");
    c->out[0] = (uint32_t)rate; c->out[1] = (uint32_t)ch; c->out[2] = 2; c->out[3] = 1; c->n_out = 4;
    snprintf(c->obj, sizeof c->obj, "audout.IAudioOut#%d", k);
    LOG("B3-AUDOUT open rate=%d channels=%d → #%d", rate, ch, k);
    return 0;
}
static int out_index(const b3_call_t *c) {
    const char *h = c->self ? strchr(c->self, '#') : NULL; int k = h ? atoi(h + 1) : -1;
    if (k < 0 || k >= MAX_OUTS || !g_out[k].used) die("B3 audout：未知输出实例 %s | unknown audio out instance", c->self ? c->self : "(null)");
    return k;
}
static uint32_t ao_state(b3_call_t *c) { c->out[0] = g_out[out_index(c)].started ? 0 : 1; c->n_out = 1; return 0; }
static uint32_t ao_start(b3_call_t *c) {
    int k = out_index(c); out_thread_start();
    pthread_mutex_lock(&g_out_lock);
    if (!g_out[k].started) { g_out[k].started = 1; if (g_out[k].qn) g_out[k].due_ns = mono_ns() + g_out[k].q[g_out[k].qh].samples * 1000000000ull / (uint64_t)g_out[k].rate; }
    pthread_mutex_unlock(&g_out_lock); return 0;
}
static uint32_t ao_stop(b3_call_t *c) { int k = out_index(c); pthread_mutex_lock(&g_out_lock); g_out[k].started = 0; pthread_mutex_unlock(&g_out_lock); return 0; }
// 3 / 7 AppendAudioOutBuffer(输入缓冲 AudioOutBuffer {下一个, 样本指针, 容量, 数据大小, 偏移}, u64 客户端指针) | (input buffer AudioOutBuffer {next, sample ptr, capacity, data size, offset}, u64 client pointer)
// 宿主出声（b3_sound_mac.c，只链进 b3host）；单测程序用这个空的弱定义 | host sound output (b3_sound_mac.c, linked into b3host only); test programs use this empty weak definition
__attribute__((weak)) void b3_sound_push(const int16_t *samples, uint64_t frames, int channels) { (void)samples; (void)frames; (void)channels; }

static uint32_t ao_append(b3_call_t *c) {
    int k = out_index(c); b3_buf_t b = b3_in_buffer(c, 0);
    if (b.size < 0x28 || c->n_in < 2) return 0x2c99;
    uint64_t size, client; memcpy(&size, (const uint8_t *)(uintptr_t)b.addr + 0x18, 8); memcpy(&client, c->in, 8);
    pthread_mutex_lock(&g_out_lock);
    if (g_out[k].qn == OUT_QUEUE) { pthread_mutex_unlock(&g_out_lock); return 0x2c99; }
    uint64_t samples = size / (2 * (uint64_t)g_out[k].channels);
    { uint64_t ptr, off; memcpy(&ptr, (const uint8_t *)(uintptr_t)b.addr + 0x8, 8); memcpy(&off, (const uint8_t *)(uintptr_t)b.addr + 0x20, 8);   // 样本指针 + 偏移（送宿主出声）| sample pointer + offset (sent to host output)
      if (ptr) b3_sound_push((const int16_t *)(uintptr_t)(ptr + off), samples, g_out[k].channels); }
    if (!g_out[k].qn && g_out[k].started) g_out[k].due_ns = mono_ns() + samples * 1000000000ull / (uint64_t)g_out[k].rate;
    g_out[k].q[(g_out[k].qh + g_out[k].qn++) % OUT_QUEUE] = (out_buf_t){client, samples};
    { static int n; if (n++ < 8) LOG("B3-AUDOUT append #%d bytes=%#llx samples=%llu started=%d queued=%d", k, (unsigned long long)size, (unsigned long long)samples, g_out[k].started, g_out[k].qn); }
    pthread_mutex_unlock(&g_out_lock); return 0;
}
static uint32_t ao_buffer_event(b3_call_t *c) { c->copy[0] = g_out[out_index(c)].ev_read; c->n_copy = 1; return 0; }
// 5 / 8 GetReleasedAudioOutBuffers(输出缓冲 u64 数组) → u32 个数 | (output buffer of u64) → u32 count
static uint32_t ao_released(b3_call_t *c) {
    int k = out_index(c); b3_buf_t b = b3_out_buffer(c, 0); unsigned cap = (unsigned)(b.size / 8), n = 0;
    if (b.size) memset((void *)(uintptr_t)b.addr, 0, b.size);   // 先清零：没取到的槽是 NULL（SDK 按 NULL 判断“没有更多”，不看个数；实测，不清零时它反复取到上次的指针）| zero first: unused slots are NULL (the SDK stops at NULL rather than using the count; measured in , without this it kept getting the previous pointer)
    pthread_mutex_lock(&g_out_lock);
    while (n < cap && n < (unsigned)g_out[k].rn) { memcpy((uint8_t *)(uintptr_t)b.addr + 8 * n, &g_out[k].rel[n], 8); n++; }
    memmove(g_out[k].rel, g_out[k].rel + n, (g_out[k].rn - n) * 8); g_out[k].rn -= n;
    pthread_mutex_unlock(&g_out_lock);
    c->out[0] = n; c->n_out = 1; return 0;
}
static uint32_t ao_contains(b3_call_t *c) {
    int k = out_index(c); uint64_t p = 0; if (c->n_in >= 2) memcpy(&p, c->in, 8); int has = 0;
    pthread_mutex_lock(&g_out_lock);
    for (int i = 0; i < g_out[k].qn; i++) if (g_out[k].q[(g_out[k].qh + i) % OUT_QUEUE].client == p) has = 1;
    pthread_mutex_unlock(&g_out_lock);
    c->out[0] = has; c->n_out = 1; return 0;
}
static uint32_t ao_count(b3_call_t *c) { c->out[0] = (uint32_t)g_out[out_index(c)].qn; c->n_out = 1; return 0; }
static uint32_t ao_played(b3_call_t *c) { uint64_t n = g_out[out_index(c)].played; memcpy(c->out, &n, 8); c->n_out = 2; return 0; }
static uint32_t ao_flush(b3_call_t *c) { c->out[0] = g_out[out_index(c)].qn > 0; c->n_out = 1; return 0; }
static uint32_t ao_set_volume(b3_call_t *c) { (void)out_index(c); return 0; }
static uint32_t ao_get_volume(b3_call_t *c) { (void)out_index(c); float v = 1.0f; memcpy(c->out, &v, 4); c->n_out = 1; return 0; }

// ---------- hwopus（硬件 Opus 解码，switchbrew「Audio services」hwopus 节）：解码出静音 ----------
// ---------- hwopus (hardware Opus decoding, switchbrew "Audio services" hwopus section): decodes to silence ----------
#define MAX_DECODERS 64
static struct { int used; int32_t rate, channels; } g_dec[MAX_DECODERS];
// 工作区大小：服务端不用这块内存，给 0x10000 页对齐（DECISION）| Work buffer size: the server does not use it; return 0x10000, page aligned (DECISION)
static uint32_t opus_work_size(b3_call_t *c) { c->out[0] = 0x10000; c->n_out = 1; return 0; }
static uint32_t opus_open(b3_call_t *c, int32_t rate, int32_t channels) {
    int k = 0; while (k < MAX_DECODERS && g_dec[k].used) k++;
    if (k == MAX_DECODERS) die("B3 hwopus：解码器实例用尽 | out of decoder instances");
    if (rate <= 0 || channels <= 0 || channels > 255) return 0x2c99;   // 参数无效 | invalid parameters
    g_dec[k] = (typeof(g_dec[k])){1, rate, channels};
    snprintf(c->obj, sizeof c->obj, "hwopus.IHardwareOpusDecoder#%d", k);
    return 0;
}
// 0 Open(s32 采样率, s32 声道 打包成 u64, u32 工作区大小, 传输内存) / 4 OpenEx(OpusParametersEx {采样率, 声道, 大帧, 填充}, u32 工作区大小, 传输内存)
// 0 Open(s32 sample rate, s32 channels packed as u64, u32 work buffer size, transfer memory) / 4 OpenEx(OpusParametersEx {rate, channels, large frame, padding}, u32 work buffer size, transfer memory)
static uint32_t opus_open_plain(b3_call_t *c) { return c->n_in >= 2 ? opus_open(c, (int32_t)c->in[0], (int32_t)c->in[1]) : 0x2c99; }
// 2 / 6 多流：参数在 0x19 输入缓冲（OpusMultiStreamParameters[Ex]，前两字是采样率与总声道）| 2 / 6 multistream: parameters in a type-0x19 input buffer (OpusMultiStreamParameters[Ex], the first two words are sample rate and total channels)
static uint32_t opus_open_multi(b3_call_t *c) {
    b3_buf_t b = b3_in_buffer(c, 0); if (b.size < 8) return 0x2c99;
    const int32_t *p = (const int32_t *)(uintptr_t)b.addr; return opus_open(c, p[0], p[1]);
}
static int dec_index(const b3_call_t *c) {
    const char *h = c->self ? strchr(c->self, '#') : NULL; int k = h ? atoi(h + 1) : -1;
    if (k < 0 || k >= MAX_DECODERS || !g_dec[k].used) die("B3 hwopus：未知解码器实例 %s | unknown decoder instance", c->self ? c->self : "(null)");
    return k;
}
// Opus 包的样本数（RFC 6716 第 3.1 节 TOC：配置号定帧长，c 位定帧数；code 3 的帧数在第二字节低 6 位），按解码器采样率换算
// Sample count of an Opus packet (RFC 6716 section 3.1 TOC: the config number gives the frame duration, the c bits the frame count; code 3 keeps the count in the low 6 bits of byte 2), scaled to the decoder rate
static int opus_samples(const uint8_t *pk, uint32_t n, int32_t rate) {
    if (!n) return 0;
    unsigned cfg = pk[0] >> 3, code = pk[0] & 3, frames = code == 0 ? 1 : code < 3 ? 2 : (n >= 2 ? pk[1] & 0x3F : 0);
    static const unsigned silk_us[4] = {10000, 20000, 40000, 60000}, celt_us[4] = {2500, 5000, 10000, 20000};
    unsigned us = cfg < 12 ? silk_us[cfg & 3] : cfg < 16 ? (cfg & 1 ? 20000 : 10000) : celt_us[cfg & 3];
    return (int)((uint64_t)frames * us * (uint64_t)rate / 1000000);
}
// 解码：输入是若干个 {u32 大端长度, u32 大端 final range, Opus 包}，本实现只取第一个（与 DecodeInterleaved 一次一个包的用法相符）；输出该包样本数 × 声道 × 2 字节的 0
// Decode: the input holds {u32 big-endian size, u32 big-endian final range, Opus packet} records; this takes the first one (matching DecodeInterleaved's one-packet-per-call use); the output is samples × channels × 2 zero bytes
static uint32_t opus_decode(b3_call_t *c, int with_perf) {
    int k = dec_index(c);
    b3_buf_t in = b3_in_buffer(c, 0), out = b3_out_buffer(c, 0);
    if (in.size < 8) return 0x2c99;
    const uint8_t *p = (const uint8_t *)(uintptr_t)in.addr;
    uint32_t len = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    if (8 + (uint64_t)len > in.size) return 0x2c99;
    int samples = opus_samples(p + 8, len, g_dec[k].rate);
    uint64_t bytes = (uint64_t)samples * g_dec[k].channels * 2;
    if (bytes > out.size) return 0x2c99;
    if (bytes) memset((void *)(uintptr_t)out.addr, 0, bytes);
    c->out[0] = 8 + len; c->out[1] = (uint32_t)samples; c->n_out = 2;
    if (with_perf) { c->out[2] = 0; c->out[3] = 0; c->n_out = 4; }   // u64 性能值，官方程序忽略 | u64 perf value, ignored by official software
    return 0;
}
static uint32_t opus_decode_old(b3_call_t *c) { return opus_decode(c, 0); }
static uint32_t opus_decode_perf(b3_call_t *c) { return opus_decode(c, 1); }
static uint32_t opus_set_context(b3_call_t *c) { (void)dec_index(c); return 0; }   // 文档：输入缓冲不使用 | docs: the input buffer is unused

const b3_cmd_t b3_audio_cmds[] = {
    {"audren:u", B3_CMIF, 0, mgr_open_renderer, "switchbrew Audio: IAudioRendererManager OpenAudioRenderer"},
    {"audren:u", B3_CMIF, 1, mgr_work_buffer_size, "switchbrew Audio: IAudioRendererManager GetWorkBufferSize"},
    {"audren:u", B3_CMIF, 2, mgr_audio_device, "switchbrew Audio: IAudioRendererManager GetAudioDeviceService"},
    {"audren:u", B3_CMIF, 4, mgr_audio_device, "switchbrew Audio: IAudioRendererManager GetAudioDeviceServiceWithRevisionInfo"},
    {"audren.IAudioRenderer", B3_CMIF, 0, r_sample_rate, "switchbrew Audio: IAudioRenderer GetSampleRate"},
    {"audren.IAudioRenderer", B3_CMIF, 1, r_sample_count, "switchbrew Audio: IAudioRenderer GetSampleCount"},
    {"audren.IAudioRenderer", B3_CMIF, 2, r_mix_buffer_count, "switchbrew Audio: IAudioRenderer GetMixBufferCount"},
    {"audren.IAudioRenderer", B3_CMIF, 3, r_state, "switchbrew Audio: IAudioRenderer GetState"},
    {"audren.IAudioRenderer", B3_CMIF, 4, r_request_update, "switchbrew Audio: IAudioRenderer RequestUpdate"},
    {"audren.IAudioRenderer", B3_CMIF, 5, r_start, "switchbrew Audio: IAudioRenderer Start"},
    {"audren.IAudioRenderer", B3_CMIF, 6, r_stop, "switchbrew Audio: IAudioRenderer Stop"},
    {"audren.IAudioRenderer", B3_CMIF, 7, r_system_event, "switchbrew Audio: IAudioRenderer QuerySystemEvent"},
    {"audren.IAudioRenderer", B3_CMIF, 8, r_set_time_limit, "switchbrew Audio: IAudioRenderer SetRenderingTimeLimit"},
    {"audren.IAudioRenderer", B3_CMIF, 9, r_get_time_limit, "switchbrew Audio: IAudioRenderer GetRenderingTimeLimit"},
    {"audren.IAudioRenderer", B3_CMIF, 10, r_request_update, "switchbrew Audio: IAudioRenderer RequestUpdateAuto"},
    {"audren.IAudioDevice", B3_CMIF, 0, dev_list_names, "switchbrew Audio: IAudioDevice ListAudioDeviceName"},
    {"audren.IAudioDevice", B3_CMIF, 1, dev_set_volume, "switchbrew Audio: IAudioDevice SetAudioDeviceOutputVolume"},
    {"audren.IAudioDevice", B3_CMIF, 2, dev_get_volume, "switchbrew Audio: IAudioDevice GetAudioDeviceOutputVolume"},
    {"audren.IAudioDevice", B3_CMIF, 3, dev_active_name, "switchbrew Audio: IAudioDevice GetActiveAudioDeviceName"},
    {"audren.IAudioDevice", B3_CMIF, 4, dev_new_event, "switchbrew Audio: IAudioDevice QueryAudioDeviceSystemEvent"},
    {"audren.IAudioDevice", B3_CMIF, 5, dev_channel_count, "switchbrew Audio: IAudioDevice GetActiveChannelCount"},
    {"audren.IAudioDevice", B3_CMIF, 6, dev_list_names, "switchbrew Audio: IAudioDevice ListAudioDeviceNameAuto"},
    {"audren.IAudioDevice", B3_CMIF, 7, dev_set_volume, "switchbrew Audio: IAudioDevice SetAudioDeviceOutputVolumeAuto"},
    {"audren.IAudioDevice", B3_CMIF, 8, dev_get_volume, "switchbrew Audio: IAudioDevice GetAudioDeviceOutputVolumeAuto"},
    {"audren.IAudioDevice", B3_CMIF, 10, dev_active_name, "switchbrew Audio: IAudioDevice GetActiveAudioDeviceNameAuto"},
    {"audren.IAudioDevice", B3_CMIF, 11, dev_new_event, "switchbrew Audio: IAudioDevice QueryAudioDeviceInputEvent"},
    {"audren.IAudioDevice", B3_CMIF, 12, dev_new_event, "switchbrew Audio: IAudioDevice QueryAudioDeviceOutputEvent"},
    {"audren.IAudioDevice", B3_CMIF, 13, dev_active_name, "switchbrew Audio: IAudioDevice GetActiveAudioOutputDeviceName"},
    {"audout:u", B3_CMIF, 0, ao_list, "switchbrew Audio: audout ListAudioOuts"},
    {"audout:u", B3_CMIF, 1, ao_open, "switchbrew Audio: audout OpenAudioOut"},
    {"audout:u", B3_CMIF, 2, ao_list, "switchbrew Audio: audout ListAudioOutsAuto"},
    {"audout:u", B3_CMIF, 3, ao_open, "switchbrew Audio: audout OpenAudioOutAuto"},
    {"audout.IAudioOut", B3_CMIF, 0, ao_state, "switchbrew Audio: IAudioOut GetAudioOutState"},
    {"audout.IAudioOut", B3_CMIF, 1, ao_start, "switchbrew Audio: IAudioOut Start"},
    {"audout.IAudioOut", B3_CMIF, 2, ao_stop, "switchbrew Audio: IAudioOut Stop"},
    {"audout.IAudioOut", B3_CMIF, 3, ao_append, "switchbrew Audio: IAudioOut AppendAudioOutBuffer"},
    {"audout.IAudioOut", B3_CMIF, 4, ao_buffer_event, "switchbrew Audio: IAudioOut RegisterBufferEvent"},
    {"audout.IAudioOut", B3_CMIF, 5, ao_released, "switchbrew Audio: IAudioOut GetReleasedAudioOutBuffers"},
    {"audout.IAudioOut", B3_CMIF, 6, ao_contains, "switchbrew Audio: IAudioOut ContainsAudioOutBuffer"},
    {"audout.IAudioOut", B3_CMIF, 7, ao_append, "switchbrew Audio: IAudioOut AppendAudioOutBufferAuto"},
    {"audout.IAudioOut", B3_CMIF, 8, ao_released, "switchbrew Audio: IAudioOut GetReleasedAudioOutBuffersAuto"},
    {"audout.IAudioOut", B3_CMIF, 9, ao_count, "switchbrew Audio: IAudioOut GetAudioOutBufferCount"},
    {"audout.IAudioOut", B3_CMIF, 10, ao_played, "switchbrew Audio: IAudioOut GetAudioOutPlayedSampleCount"},
    {"audout.IAudioOut", B3_CMIF, 11, ao_flush, "switchbrew Audio: IAudioOut FlushAudioOutBuffers"},
    {"audout.IAudioOut", B3_CMIF, 12, ao_set_volume, "switchbrew Audio: IAudioOut SetAudioOutVolume"},
    {"audout.IAudioOut", B3_CMIF, 13, ao_get_volume, "switchbrew Audio: IAudioOut GetAudioOutVolume"},
    {"hwopus", B3_CMIF, 0, opus_open_plain, "switchbrew Audio: hwopus OpenHardwareOpusDecoder"},
    {"hwopus", B3_CMIF, 1, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSize"},
    {"hwopus", B3_CMIF, 2, opus_open_multi, "switchbrew Audio: hwopus OpenHardwareOpusDecoderForMultiStream"},
    {"hwopus", B3_CMIF, 3, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSizeForMultiStream"},
    {"hwopus", B3_CMIF, 4, opus_open_plain, "switchbrew Audio: hwopus OpenHardwareOpusDecoderEx"},
    {"hwopus", B3_CMIF, 5, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSizeEx"},
    {"hwopus", B3_CMIF, 6, opus_open_multi, "switchbrew Audio: hwopus OpenHardwareOpusDecoderForMultiStreamEx"},
    {"hwopus", B3_CMIF, 7, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSizeForMultiStreamEx"},
    {"hwopus", B3_CMIF, 8, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSizeExEx"},
    {"hwopus", B3_CMIF, 9, opus_work_size, "switchbrew Audio: hwopus GetWorkBufferSizeForMultiStreamExEx"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 0, opus_decode_old, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleavedOld"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 1, opus_set_context, "switchbrew Audio: IHardwareOpusDecoder SetContext"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 2, opus_decode_old, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleavedForMultiStreamOld"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 3, opus_set_context, "switchbrew Audio: IHardwareOpusDecoder SetContextForMultiStream"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 4, opus_decode_perf, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleavedWithPerfOld"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 5, opus_decode_perf, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleavedForMultiStreamWithPerfOld"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 8, opus_decode_perf, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleaved"},
    {"hwopus.IHardwareOpusDecoder", B3_CMIF, 9, opus_decode_perf, "switchbrew Audio: IHardwareOpusDecoder DecodeInterleavedForMultiStream"},
};
const unsigned b3_audio_ncmds = sizeof b3_audio_cmds / sizeof *b3_audio_cmds;
