// B3 的最小 Mac 前端：B3_WINDOW=1 时开一个 1280x720 窗口，内容视图挂 CAMetalLayer 供通用层的 Metal 层出图；客体在 64 MiB 栈的线程上跑，主线程跑 NSApp。
// 出图、关闭，键盘与手柄输入（按键与摇杆，经 b3_hid.c 的掌机手柄模拟），鼠标左键当触屏；不改通用层的 front_mac.m。没设 B3_WINDOW 时与无窗口相同。
// B3's minimal Mac front end : with B3_WINDOW=1 open a 1280x720 window whose content view hosts a CAMetalLayer for the base runtime's Metal layer to present to; the guest runs on a thread with a 64 MiB stack and the main thread runs NSApp.
// Presentation, closing, keyboard and gamepad input (buttons and sticks, through b3_hid.c's handheld emulation), left mouse as touch ; the base runtime's front_mac.m is untouched. Without B3_WINDOW it behaves as windowless.
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include "dm.h"

static CAMetalLayer *g_layer;
void *front_metal_layer(void) { return (__bridge void *)g_layer; }

@interface B3WinDelegate : NSObject <NSWindowDelegate> @end
@implementation B3WinDelegate
- (void)windowWillClose:(NSNotification *)n { (void)n; LOG("B3 窗口关闭，退出 | window closed, exiting"); exit(0); }
- (void)windowDidResize:(NSNotification *)n { NSWindow *w = n.object; g_layer.frame = w.contentView.bounds; g_layer.drawableSize = CGSizeMake(w.contentView.bounds.size.width * w.backingScaleFactor, w.contentView.bounds.size.height * w.backingScaleFactor); }
@end

static void *guest_tramp(void *p) {
    static char alt[65536];                       // 信号备用栈是每线程的，崩溃打印要在客体线程上也生效 | the signal alternate stack is per thread; crash printing must work on the guest thread too
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((void (*)(void))p)(); LOG("B3 客体线程结束 | guest thread finished"); exit(0);
}

void b3_front_run(void (*guest)(void)) {
    const char *e = getenv("B3_WINDOW");
    if (!e || strcmp(e, "1")) { guest(); return; }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 720)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @"b3host"; win.restorable = NO; win.contentAspectRatio = NSMakeSize(16, 9);
        // 全屏：F11 或窗口绿色按钮切换；B3_FULLSCREEN=1 启动即全屏。全屏时画面按屏幕实际像素输出（1080p 屏 = 1920×1080），配合 MetalFX 放大
        // Full screen: toggle with F11 or the window's green button; B3_FULLSCREEN=1 starts full screen. Full screen outputs at the screen's real pixels (1920×1080 on a 1080p screen), together with MetalFX upscaling
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        { NSRect vf = NSScreen.mainScreen.visibleFrame; CGFloat w = fmin(1600, vf.size.width * 0.9), h = w * 9 / 16; if (h > vf.size.height * 0.9) { h = vf.size.height * 0.9; w = h * 16 / 9; } [win setContentSize:NSMakeSize(floor(w), floor(h))]; }   // 缺省窗口尽量接近 1600×900 | default window as close to 1600×900 as fits
        static B3WinDelegate *dg; dg = [B3WinDelegate new]; win.delegate = dg;
        NSView *v = win.contentView; v.wantsLayer = YES; v.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        g_layer.frame = v.bounds;
        g_layer.drawableSize = CGSizeMake(v.bounds.size.width * win.backingScaleFactor, v.bounds.size.height * win.backingScaleFactor);
        [v.layer addSublayer:g_layer];
        [win center]; [win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
        { const char *fs = getenv("B3_FULLSCREEN"); if (fs && !strcmp(fs, "1")) dispatch_async(dispatch_get_main_queue(), ^{ [win toggleFullScreen:nil]; }); }
        // 键盘 → 掌机手柄（b3_hid.c 的 b3_input_set）：键位见下方按键表，H = Home，F12 = 抓帧。另外每 1/60 秒轮询已连的手柄（GameController，按位置映射：下 = B、右 = A、左 = Y、上 = X），与键盘合并。
        // Keyboard → handheld controller (b3_input_set in b3_hid.c): key map in the switch table below , H = Home, F12 = grab. A connected gamepad is also polled every 1/60 s (GameController, positional mapping: bottom = B, right = A, left = Y, top = X) and merged with the keyboard.
        static uint64_t held; static unsigned sticks;   // sticks 位：0 W 1 A 2 S 3 D 4 G 5 V 6 B 7 N | sticks bits: 0 W 1 A 2 S 3 D 4 G 5 V 6 B 7 N
        static void (^push)(void) = ^{
            uint64_t b = held; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
            if (sticks & 8) lx += 32767; if (sticks & 2) lx -= 32767; if (sticks & 1) ly += 32767; if (sticks & 4) ly -= 32767;
            if (sticks & 128) rx += 32767; if (sticks & 32) rx -= 32767; if (sticks & 16) ry += 32767; if (sticks & 64) ry -= 32767;
            GCExtendedGamepad *g = GCController.controllers.firstObject.extendedGamepad;
            // 手柄按键直接用原始状态（输入记录显示有线 Xbox 手柄没有触点抖动；之前加的 30 ms 去抖把每次按下拉长，
            // 主菜单自带的按住连发约 200 ms 起跳，被拉长的按下更容易越过它，反而一按两下）
            // Gamepad buttons use the raw state (the input log showed no contact bounce on the wired Xbox controller; the 30 ms debounce added earlier lengthened every press,
            // and the main menu's own hold-to-repeat starts at about 200 ms, so lengthened presses crossed it more often and caused double steps)
            uint64_t raw = 0;
            if (g) {
                if (g.buttonB.pressed) raw |= 1ull << 0; if (g.buttonA.pressed) raw |= 1ull << 1; if (g.buttonY.pressed) raw |= 1ull << 2; if (g.buttonX.pressed) raw |= 1ull << 3;
                if (g.leftThumbstickButton.pressed) raw |= 1ull << 4; if (g.rightThumbstickButton.pressed) raw |= 1ull << 5;
                if (g.leftShoulder.pressed) raw |= 1ull << 6; if (g.rightShoulder.pressed) raw |= 1ull << 7; if (g.leftTrigger.pressed) raw |= 1ull << 8; if (g.rightTrigger.pressed) raw |= 1ull << 9;
                if (g.buttonMenu.pressed) raw |= 1ull << 10; if (g.buttonOptions.pressed) raw |= 1ull << 11;
                // Home（Xbox 键）：按下沿切换主页 / 游戏（b3_applet_home）；不进手柄按键位 | Home (Xbox button): a press edge toggles HOME menu / game (b3_applet_home); not a gamepad button bit
                { static int hl; int h = g.buttonHome.pressed; if (h && !hl) { void b3_applet_home(void); b3_applet_home(); } hl = h; }
                if (g.dpad.left.pressed) raw |= 1ull << 12; if (g.dpad.up.pressed) raw |= 1ull << 13; if (g.dpad.right.pressed) raw |= 1ull << 14; if (g.dpad.down.pressed) raw |= 1ull << 15;
            }
            // 十字键短按整形（主菜单的按住连发按帧计数，窗口下跑 60 帧时约 110 ms 就起跳；手柄短按 120–145 ms 常被当成长按，键盘轻点更短所以没事）：按住 90–500 ms 之间对游戏报松开，超过 500 ms 再报按下（之后由游戏自己的连发接管）
            // D-pad tap shaping (the main menu counts its hold-to-repeat in frames and at 60 fps in the window starts after about 110 ms; gamepad taps of 120–145 ms were often taken as holds, keyboard taps are shorter): between 90 and 500 ms of holding the game sees a release, past 500 ms a press again (the game's own repeat takes over from there)
            uint64_t phys = raw;   // 整形前的物理状态（输入记录用）| physical state before shaping (for the input log)
            { static uint64_t since[4]; uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
              for (int k = 0; k < 4; k++) { uint64_t m = 1ull << (12 + k);
                  if (!(raw & m)) { since[k] = 0; continue; }
                  if (!since[k]) since[k] = now;
                  uint64_t held = now - since[k]; if (held >= 90000000ull && held < 500000000ull) raw &= ~m; } }
            b |= raw;
            if (g) {
                // 摇杆死区 0.2（径向）：静止时的漂移不当成推杆| stick deadzone 0.2 (radial): resting drift is not taken as a push
                float ax = g.leftThumbstick.xAxis.value, ay = g.leftThumbstick.yAxis.value, bx = g.rightThumbstick.xAxis.value, by = g.rightThumbstick.yAxis.value;
                if (ax * ax + ay * ay < 0.04f) ax = ay = 0; if (bx * bx + by * by < 0.04f) bx = by = 0;
                if (!lx && !ly) { lx = (int32_t)(ax * 32767); ly = (int32_t)(ay * 32767); }
                if (!rx && !ry) { rx = (int32_t)(bx * 32767); ry = (int32_t)(by * 32767); }
            }
            { static int on = -1; static uint64_t lb, lraw; static int32_t llx, lly; if (on < 0) { const char *e = getenv("B3_INPUTLOG"); on = e && !strcmp(e, "1"); }
              static uint64_t lphys; if (on && (b != lb || raw != lraw || phys != lphys || lx / 4096 != llx / 4096 || ly / 4096 != lly / 4096)) {   // B3_INPUTLOG=1：输入每次变化记一行（原始手柄位、去抖后按键、左摇杆）| B3_INPUTLOG=1: one line per input change (raw gamepad bits, debounced buttons, left stick)
                  LOG("B3-INPUT t=%.3f phys=%#llx raw=%#llx buttons=%#llx lx=%d ly=%d gstick=%.2f,%.2f", clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, (unsigned long long)phys, (unsigned long long)raw, (unsigned long long)b, lx, ly,
                      g ? g.leftThumbstick.xAxis.value : 0.f, g ? g.leftThumbstick.yAxis.value : 0.f);
                  lb = b; lraw = raw; lphys = phys; llx = lx; lly = ly; } }
            void b3_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry); b3_input_set(b, lx, ly, rx, ry);
        };
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *ev) {
            if (ev.window != win || (ev.modifierFlags & NSEventModifierFlagCommand)) return ev;
            int bit = -1, st = -1;
            // 键位：A/B = L/K，X/Y = I/J，L/R = Q/E，ZL/ZR = Z/空格，+/− = = / -，回车 = A，退格 = B，L3/R3 = U/O，十字键 = 方向键，左摇杆 = WASD，右摇杆 = G（上）V（左）B（下）N（右）
            // Key map : A/B = L/K, X/Y = I/J, L/R = Q/E, ZL/ZR = Z/Space, +/− = = / -, Return = A, Backspace = B, L3/R3 = U/O, D-pad = arrow keys, left stick = WASD, right stick = G (up) V (left) B (down) N (right)
            switch (ev.keyCode) { case 37: case 36: bit = 0; break; case 40: case 51: bit = 1; break; case 34: bit = 2; break; case 38: bit = 3; break;
                                  case 32: bit = 4; break; case 31: bit = 5; break;
                                  case 12: bit = 6; break; case 14: bit = 7; break; case 6: bit = 8; break; case 49: bit = 9; break; case 24: bit = 10; break; case 27: bit = 11; break;
                                  case 123: bit = 12; break; case 126: bit = 13; break; case 124: bit = 14; break; case 125: bit = 15; break;
                                  case 13: st = 0; break; case 0: st = 1; break; case 1: st = 2; break; case 2: st = 3; break;
                                  case 5: st = 4; break; case 9: st = 5; break; case 11: st = 6; break; case 45: st = 7; break; }
            if (ev.keyCode == 103) { if (ev.type == NSEventTypeKeyDown && !ev.isARepeat) [win toggleFullScreen:nil]; return nil; }   // F11：全屏切换 | F11: toggle full screen
            if (ev.keyCode == 111 && ev.type == NSEventTypeKeyDown) {   // F12：抓下一帧（通道记录 + 每个通道的颜色快照 + 画面），存到 ~/b3-work/play/cap/grab_<帧号>（诊断）| F12: grab the next frame (pass dump + per-pass colour snapshots + picture) into ~/b3-work/play/cap/grab_<frame> (diagnostic)
                extern _Atomic uint64_t g_b3_dump_at, g_b3_frame_now; uint64_t f = atomic_load(&g_b3_frame_now) + 3; atomic_store(&g_b3_dump_at, f);
                LOG("B3-GRAB 抓第 %llu 帧 | grabbing frame %llu", (unsigned long long)f, (unsigned long long)f); return nil; }
            if (ev.keyCode == 4) { if (ev.type == NSEventTypeKeyDown && !ev.isARepeat) { void b3_applet_home(void); b3_applet_home(); } return nil; }   // H = Home（切主页 / 回游戏）| H = Home (HOME menu / back to the game)
            if (bit < 0 && st < 0) return ev;
            int down = ev.type == NSEventTypeKeyDown;
            if (bit >= 0) { if (down) held |= 1ull << bit; else held &= ~(1ull << bit); }
            else { if (down) sticks |= 1u << st; else sticks &= ~(1u << st); }
            push();
            return nil;
        }];
        // 鼠标左键 → 触屏（游戏原生支持触屏点击；窗口内容按 16:9 线性换算到 1280×720）| left mouse → touch screen (the game supports touch natively; window content maps linearly to 1280×720 at 16:9)
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp handler:^NSEvent *(NSEvent *ev) {
            if (ev.window != win) return ev;
            void b3_touch_set(int n, const uint32_t *ids, const float *xs, const float *ys);
            NSPoint p = [v convertPoint:ev.locationInWindow fromView:nil]; NSSize sz = v.bounds.size;
            uint32_t id = 1; float x = (float)(p.x / sz.width * 1280), y = (float)((1 - p.y / sz.height) * 720);
            b3_touch_set(ev.type == NSEventTypeLeftMouseUp ? 0 : 1, &id, &x, &y);
            return ev;
        }];
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *t) { (void)t; if (GCController.controllers.count) push(); }];   // 120 Hz 轮询（短按整形的时间点也靠它）| 120 Hz polling (also drives the tap-shaping time points)
        // 手柄状态一变就立即推送（不等轮询）| push as soon as the gamepad state changes (without waiting for polling)
        // Home 键关掉系统手势（否则 macOS 拿去开游戏中心 / 启动台，应用收不到）| turn off the system gesture on Home (otherwise macOS takes it for Game Center / Launchpad and the app never sees it)
        void (^hook)(GCController *) = ^(GCController *c) { c.extendedGamepad.buttonHome.preferredSystemGestureState = GCSystemGestureStateDisabled; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) { (void)gp; (void)el; push(); }; };
        for (GCController *c in GCController.controllers) hook(c);
        [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { hook(n.object); }];
        // 窗口标题每秒显示帧率（出图次数差）| the window title shows the frame rate every second (difference of present counts)
        [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) { (void)t; extern _Atomic uint64_t g_b3_presents; static uint64_t last;
            uint64_t now = atomic_load(&g_b3_presents); win.title = [NSString stringWithFormat:@"b3host — %llu fps", (unsigned long long)(now - last)]; last = now; }];
        LOG("B3 窗口已开 %.0fx%.0f | window open", win.contentView.bounds.size.width, win.contentView.bounds.size.height);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, (void *)guest)) die("B3 客体线程创建失败 | guest thread creation failed");
        [NSApp run];
    }
}
