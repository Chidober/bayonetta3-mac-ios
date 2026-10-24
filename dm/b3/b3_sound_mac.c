// B3 的 Mac 声音输出：audout 收到的 PCM16 交错样本（48 kHz）写进环形缓冲，CoreAudio 的默认输出单元在回调里取走播放。
// 缓冲的释放时机仍由 b3_audio.c 按时长计时（游戏节奏不变）；这里只负责出声：游戏写得比设备快时丢最旧的数据，把延迟压在约 150 ms 内，读空时补静音。
// B3_MUTE=1 关闭。只链进 b3host 与 iOS 版；单测程序用 b3_audio.c 里的空弱定义。iOS 上用 RemoteIO 输出单元（没有 DefaultOutput），回调与格式相同。
// B3 Mac sound output : PCM16 interleaved samples (48 kHz) received by audout go into a ring buffer, and CoreAudio's default output unit takes them in its callback and plays them.
// Buffer release timing is still clocked by b3_audio.c from each buffer's duration (game pacing unchanged); this file only makes the sound: when the game writes faster than the device, the oldest data is dropped to keep latency around 150 ms, and an empty ring plays silence.
// B3_MUTE=1 disables it. Linked into b3host and the iOS app only; test programs use the empty weak definition in b3_audio.c. iOS uses the RemoteIO output unit (there is no DefaultOutput), with the same callback and format.
#include "dm.h"
#include <AudioToolbox/AudioToolbox.h>
#include <stdatomic.h>
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#include <CoreAudio/CoreAudio.h>
#endif

#define RING_FRAMES 65536                  // 约 1.36 s（2 的幂）| about 1.36 s (power of two)
#define MAX_LAG (48000 * 150 / 1000)       // 最大延迟 150 ms | maximum latency 150 ms
static int16_t g_ring[RING_FRAMES * 2];
static _Atomic uint64_t g_w, g_r;          // 写 / 读位置（帧，单调增长）| write / read positions (frames, monotonic)
static AudioComponentInstance g_unit;
static _Atomic uint64_t g_under, g_drop;   // 设备要数据时缓冲不够补的静音帧数、超延迟丢掉的帧数（音画同步诊断）| silent frames filled when the device asked for more than the ring held, frames dropped over the latency cap (A/V sync diagnostics)

static OSStatus render(void *ref, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)fl; (void)ts; (void)bus;
    int16_t *out = io->mBuffers[0].mData; uint64_t r = atomic_load(&g_r), w = atomic_load(&g_w);
    UInt32 n = (UInt32)(w - r < frames ? w - r : frames);
    for (UInt32 i = 0; i < n; i++) { uint64_t k = (r + i) & (RING_FRAMES - 1); out[2 * i] = g_ring[2 * k]; out[2 * i + 1] = g_ring[2 * k + 1]; }
    memset(out + 2 * n, 0, (frames - n) * 4);
    if (n < frames && w > 0) atomic_fetch_add(&g_under, frames - n);
    atomic_store(&g_r, r + n);
    return noErr;
}

static int open_unit(void) {
#if TARGET_OS_IPHONE
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
#else
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
#endif
    AudioComponent comp = AudioComponentFindNext(NULL, &d);
    if (!comp || AudioComponentInstanceNew(comp, &g_unit)) return 0;
    AudioStreamBasicDescription f = {48000, kAudioFormatLinearPCM, kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    AURenderCallbackStruct cb = {render, NULL};
    if (AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(g_unit) || AudioOutputUnitStart(g_unit)) return 0;
    return 1;
}

// 输出设备延迟（秒）：从渲染回调取走样本到声音真正出来（含蓝牙耳机）。Mac 查默认输出设备，iOS 由 b3_front_ios.m 用 AVAudioSession 提供
// Output device latency (seconds): from the render callback taking samples to the sound actually coming out (Bluetooth headphones included). Mac queries the default output device; iOS is provided by b3_front_ios.m through AVAudioSession
__attribute__((weak)) double b3_sound_device_latency(void) {
#if TARGET_OS_IPHONE
    return 0;
#else
    AudioObjectID dev = 0; UInt32 sz = sizeof dev; AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) || !dev) return 0;
    UInt32 lat = 0, safe = 0, buf = 0, srate_sz = sizeof(Float64); Float64 rate = 48000; sz = sizeof(UInt32);
    a.mScope = kAudioObjectPropertyScopeOutput;
    a.mSelector = kAudioDevicePropertyLatency; AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &lat);
    a.mSelector = kAudioDevicePropertySafetyOffset; sz = sizeof(UInt32); AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &safe);
    a.mSelector = kAudioDevicePropertyBufferFrameSize; sz = sizeof(UInt32); AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &buf);
    a.mSelector = kAudioDevicePropertyNominalSampleRate; AudioObjectGetPropertyData(dev, &a, 0, NULL, &srate_sz, &rate);
    return rate > 0 ? (lat + safe + buf) / rate : 0;
#endif
}
static _Atomic uint64_t g_lat_frames;   // 设备延迟（帧），只记录用 | device latency (frames), for logging only

// b3_audio.c 在 AppendAudioOutBuffer 时调用：samples 是 frames 帧、channels 声道的 PCM16 交错数据
// called by b3_audio.c on AppendAudioOutBuffer: samples is frames frames of interleaved PCM16 with channels channels
void b3_sound_push(const int16_t *samples, uint64_t frames, int channels) {
    static int state;   // 0 未开、1 开了、-1 关闭或失败 | 0 not opened, 1 open, -1 disabled or failed
    if (!state) { const char *m = getenv("B3_MUTE"); state = m && !strcmp(m, "1") ? -1 : open_unit() ? 1 : -1; LOG("B3-SOUND %s", state > 0 ? "CoreAudio 输出已开 | CoreAudio output open" : "不出声 | muted");
                  if (state > 0) { double l = b3_sound_device_latency(); atomic_store(&g_lat_frames, (uint64_t)(l * 48000)); LOG("B3-SOUND 设备延迟 %.1f ms | device latency", l * 1000); } }
    if (state < 0 || !samples || channels < 1) return;
    uint64_t w = atomic_load(&g_w);
    for (uint64_t i = 0; i < frames; i++) {
        uint64_t k = (w + i) & (RING_FRAMES - 1);
        g_ring[2 * k] = samples[i * channels]; g_ring[2 * k + 1] = samples[i * channels + (channels > 1)];
    }
    atomic_store(&g_w, w + frames);
    { static uint64_t acc; static int peak, nrep; for (uint64_t i = 0; i < frames * (uint64_t)channels; i++) { int v = samples[i] < 0 ? -samples[i] : samples[i]; if (v > peak) peak = v; }
      acc += frames; if (acc >= 48000 * 5) { atomic_store(&g_lat_frames, (uint64_t)(b3_sound_device_latency() * 48000));   // 设备延迟每 5 s 重取（换耳机会变）| device latency re-read every 5 s (changes when headphones switch)
          if (nrep++ < 2000) LOG("B3-SOUND 5 s 峰值 %d，欠载补静音 %llu 帧，超延迟丢弃 %llu 帧，当前缓冲 %llu ms，设备延迟 %llu ms | 5 s peak, underrun silence frames, frames dropped over latency, current buffer, device latency",
          peak, (unsigned long long)atomic_exchange(&g_under, 0), (unsigned long long)atomic_exchange(&g_drop, 0), (unsigned long long)((atomic_load(&g_w) - atomic_load(&g_r)) * 1000 / 48000), (unsigned long long)(atomic_load(&g_lat_frames) * 1000 / 48000)); acc = 0; peak = 0; } }   // 电平记录（确认不是静音）| level log (confirms it is not silence)
    // 积压回落（过场声音很大的固定延迟）：换输出设备（如连上 AirPods）时设备停顿一下，积压涨到 150 ms 后不会自己回落——生产与消费一样快。
    // 积压连续 0.5 s 超过 90 ms，就跳过最旧的数据回到 50 ms（游戏自己只排约 42 ms）
    // Backlog recovery (large fixed sound delay in cutscenes): when the output device changes (e.g. AirPods connect) the device stalls briefly, the backlog grows to 150 ms and never drains by itself -- production and consumption run at the same speed.
    // If the backlog stays above 90 ms for 0.5 s straight, skip the oldest data back to 50 ms (the game itself queues only about 42 ms)
    { static uint64_t over; uint64_t lvl = w + frames - atomic_load(&g_r);
      if (lvl > 48000 * 90 / 1000) { if ((over += frames) > 24000) { uint64_t nr = w + frames - 48000 * 50 / 1000; atomic_fetch_add(&g_drop, nr - atomic_load(&g_r)); atomic_store(&g_r, nr); over = 0;
                                                                       LOG("B3-SOUND 积压 %llu ms 回落到 50 ms | backlog back to 50 ms", (unsigned long long)(lvl * 1000 / 48000)); } }
      else over = 0; }
    uint64_t r = atomic_load(&g_r); if (w + frames - r > MAX_LAG) { atomic_fetch_add(&g_drop, w + frames - MAX_LAG - r); atomic_store(&g_r, w + frames - MAX_LAG); }   // 超出最大延迟：跳过最旧的 | over the maximum latency: skip the oldest
}
