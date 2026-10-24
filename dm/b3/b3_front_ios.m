// B3 的最小 iOS 前端：全屏横屏窗口，视图的底层就是 CAMetalLayer 供通用层的 Metal 层出图；客体在 64 MiB 栈的线程上跑，主线程跑 UIKit（场景生命周期，iOS 27 起必需）。
// 手柄（GameController，按位置映射与 Mac 前端相同：下 = B、右 = A、左 = Y、上 = X；Home 键切主页 / 游戏）与触屏操控层（B3TouchView）合并；接上手柄时触屏层自动隐藏。
// B3's minimal iOS front end : a full-screen landscape window whose view's backing layer is a CAMetalLayer for the base runtime's Metal layer to present to; the guest runs on a thread with a 64 MiB stack and the main thread runs UIKit (scene life cycle, required from iOS 27).
// Gamepad (GameController, same positional mapping as the Mac front end: bottom = B, right = A, left = Y, top = X; the Home button toggles HOME menu / game) merged with the touch control layer (B3TouchView); the touch layer hides itself while a gamepad is connected.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include <signal.h>
#include "dm.h"

static CAMetalLayer *g_layer;
static void (*g_guest)(void);
void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 输出设备延迟（秒，含蓝牙耳机），b3_sound_mac.c 用它把游戏看到的播放进度对齐到耳朵听到的时刻 | output device latency (seconds, Bluetooth headphones included); b3_sound_mac.c uses it to align the playback progress the game sees with when the ear hears it
double b3_sound_device_latency(void) { AVAudioSession *s = AVAudioSession.sharedInstance; return s.outputLatency + s.IOBufferDuration; }
void b3_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void b3_applet_home(void);
// 触屏层的输出（主线程写、push 读，都在主线程）：按键位同 b3_input_set，摇杆 −32767..32767 | touch layer output (written and read on the main thread): button bits as in b3_input_set, sticks −32767..32767
static uint64_t g_touch_btn; static int32_t g_touch_lx, g_touch_ly, g_touch_rx, g_touch_ry;

static void *guest_tramp(void *p) {
    static char alt[65536];                       // 信号备用栈是每线程的，崩溃打印要在客体线程上也生效 | the signal alternate stack is per thread; crash printing must work on the guest thread too
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((void (*)(void))p)(); LOG("B3 客体线程结束 | guest thread finished"); exit(0);
}

// 手柄 → 掌机手柄（b3_hid.c 的 b3_input_set）：原始按键、十字键短按整形、摇杆径向死区 0.2，与 b3_front_mac.m 相同（依据见那里的注释）
// Gamepad → handheld controller (b3_input_set in b3_hid.c): raw buttons, D-pad tap shaping, radial stick deadzone 0.2, the same as b3_front_mac.m (reasons in the comments there)
static void push(void) {
    GCExtendedGamepad *g = GCController.controllers.firstObject.extendedGamepad;
    uint64_t raw = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    if (g) {
        if (g.buttonB.pressed) raw |= 1ull << 0; if (g.buttonA.pressed) raw |= 1ull << 1; if (g.buttonY.pressed) raw |= 1ull << 2; if (g.buttonX.pressed) raw |= 1ull << 3;
        if (g.leftThumbstickButton.pressed) raw |= 1ull << 4; if (g.rightThumbstickButton.pressed) raw |= 1ull << 5;
        if (g.leftShoulder.pressed) raw |= 1ull << 6; if (g.rightShoulder.pressed) raw |= 1ull << 7; if (g.leftTrigger.pressed) raw |= 1ull << 8; if (g.rightTrigger.pressed) raw |= 1ull << 9;
        if (g.buttonMenu.pressed) raw |= 1ull << 10; if (g.buttonOptions.pressed) raw |= 1ull << 11;
        { static int hl; int h = g.buttonHome.pressed; if (h && !hl) b3_applet_home(); hl = h; }   // Home：按下沿切换 | Home: a press edge toggles
        if (g.dpad.left.pressed) raw |= 1ull << 12; if (g.dpad.up.pressed) raw |= 1ull << 13; if (g.dpad.right.pressed) raw |= 1ull << 14; if (g.dpad.down.pressed) raw |= 1ull << 15;
        float ax = g.leftThumbstick.xAxis.value, ay = g.leftThumbstick.yAxis.value, bx = g.rightThumbstick.xAxis.value, by = g.rightThumbstick.yAxis.value;
        if (ax * ax + ay * ay < 0.04f) ax = ay = 0; if (bx * bx + by * by < 0.04f) bx = by = 0;
        lx = (int32_t)(ax * 32767); ly = (int32_t)(ay * 32767); rx = (int32_t)(bx * 32767); ry = (int32_t)(by * 32767);
    }
    { static uint64_t since[4]; uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
      for (int k = 0; k < 4; k++) { uint64_t m = 1ull << (12 + k);
          if (!(raw & m)) { since[k] = 0; continue; }
          if (!since[k]) since[k] = now;
          uint64_t held = now - since[k]; if (held >= 90000000ull && held < 500000000ull) raw &= ~m; } }
    raw |= g_touch_btn;
    if (!lx && !ly) { lx = g_touch_lx; ly = g_touch_ly; }
    if (!rx && !ry) { rx = g_touch_rx; ry = g_touch_ry; }
    b3_input_set(raw, lx, ly, rx, ry);
}

// ---------- 触屏操控层（第一版）----------
// 按游戏动作摆的按钮，位置大小按用户设计稿（k_btn）。
// 左下区按下 = 浮动左摇杆（移动）；左下区以外没按到按钮的滑动 = 视角（按手指速度换算右摇杆）。摇杆半径 56 点，死区 0.15。
// 透明度用上方正中的 ◐ 循环（存 NSUserDefaults）。布局拖动微调留到帧率达标后（TASKS ）。
// ---------- Touch control layer (first version) ----------
// Buttons laid out by game action, positions and sizes from the user's design (k_btn).
// A press in the lower-left zone = floating left stick (move); a swipe outside the lower-left zone that misses every button = camera (finger speed becomes the right stick). Stick radius 56 points, deadzone 0.15.
// Opacity cycles with ◐ at the top centre (saved in NSUserDefaults). Drag-to-adjust layout waits until the frame rate is met (TASKS ).
// 布局按用户给的设计稿（852×393 点画布，左上角 x / y + 宽高；rect = 圆角矩形，否则圆形）；别的屏幕尺寸按宽高等比换算位置
// Layout follows the user's design (an 852×393-point canvas, top-left x / y + width / height; rect = rounded rectangle, otherwise a circle); on other screen sizes positions scale with width / height
#define DESIGN_W 852.0
#define DESIGN_H 393.0
typedef struct { const char *label; int bit; float x, y, w, h; int rect, in_menu; } tbtn_t;
static const tbtn_t k_btn[] = {   // in_menu：0 只在战斗模式、1 两种模式都有、2 只在菜单模式 | in_menu: 0 combat mode only, 1 both modes, 2 menu mode only
    {"🔫 Y", 3, 660, 164, 65, 65}, {"🦶 A", 0, 601, 305, 70, 70}, {"👊 X", 2, 629, 230, 64, 64}, {"🦋 B", 1, 714, 212, 64, 64}, {"💨 ZR", 9, 691, 287, 75, 75},
    {"🌛 ZL", 8, 84, 143, 56, 56}, {"R3", 5, 729, 68, 44, 44}, {"R", 7, 786, 60, 56, 56}, {"L", 6, 12, 84, 44, 44}, {"L3", 4, 70, 79, 44, 44},
    {"−", 11, 392, 8, 44, 28, 1}, {"+", 10, 488, 8, 44, 28, 1, 0}, {"+", 10, 678, 9, 76, 49, 1, 2},
    {"L", 6, 18, 54, 67, 46, 1, 2}, {"R", 7, 765, 51, 70, 47, 1, 2}, {"ZL", 8, 18, 108, 67, 46, 1, 2}, {"ZR", 9, 765, 106, 70, 47, 1, 2},   // 菜单模式的 L / R / ZL / ZR（角色菜单等按 L / R 翻页，；ZL / ZR 在 L / R 下面）| L / R / ZL / ZR in menu mode (the character menu etc. turn pages with L / R, ; ZL / ZR below L / R)
};
static const CGRect k_dpad_rect = {{166, 131}, {90, 90}}, k_zone = {{0, 236}, {256, 157}};
static const CGRect k_menu_btn = {{328, 8}, {56, 28}}, k_menu_btn_m = {{698, 182}, {56, 28}}, k_alpha_btn = {{444, 8}, {36, 28}}, k_back_btn = {{762, 163}, {83, 51}};   // _m：菜单模式里的位置 | _m: position in menu mode
#define NBTN (int)(sizeof k_btn / sizeof k_btn[0])
_Static_assert(sizeof k_btn / sizeof k_btn[0] <= 32, "k_btn 超过 _btn / _owner 的容量 | k_btn exceeds the _btn / _owner capacity");
static const struct { int bit; float dx, dy; } k_dpad[] = {{12, -1, 0}, {13, 0, -1}, {14, 1, 0}, {15, 0, 1}};   // 方向（乘以十字键半宽的 2/3）| directions (times 2/3 of the D-pad half width)

@interface B3TouchView : UIView @end
@implementation B3TouchView {
    CAShapeLayer *_btn[32], *_dp[4], *_stk[2][2]; UITouch *_owner[32], *_dpOwner, *_stkT[2]; CGPoint _stkO[2]; float _alpha; UIButton *_opa;
    UIImpactFeedbackGenerator *_hap; NSTimeInterval _stkT0[2];
    CGPoint _camPrev, _camV;
    // 菜单模式（游戏原生不支持触屏；菜单里不要战斗按键，只要轻点和返回）：轻点 = A，滑动 = 十字键一格，“B” 键 = B；右上角按钮手动切换
    // Menu mode (the game has no native touch support; menus need no combat buttons, only tap and back): tap = A, swipe = one D-pad step, the "B" button = B; switched by hand with the button at the top
    BOOL _menu; uint64_t _pulse; UIButton *_mode, *_back; UITouch *_mt; CGPoint _mO; NSTimeInterval _mT0;   // 滑动视角：上一拍位置、当前右摇杆量（−1..1）| swipe camera: position at the previous tick, current right-stick amount (−1..1)
    BOOL _zlLatch;   // 魔兽键是开关：点一下按住 ZL，再点一下松开（魔兽模式下 XYAB 照常用）| the demon button toggles: one tap holds ZL, the next releases it (XYAB work as usual in demon mode)
}
- (instancetype)initWithFrame:(CGRect)f {
    if (!(self = [super initWithFrame:f])) return nil;
    self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor;
    NSNumber *a = [NSUserDefaults.standardUserDefaults objectForKey:@"b3_touch_alpha"]; _alpha = a ? a.floatValue : 0.35f;
    _hap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    for (int i = 0; i < NBTN; i++) _btn[i] = k_btn[i].rect ? [self box:CGSizeMake(k_btn[i].w, k_btn[i].h) label:k_btn[i].label] : [self ring:k_btn[i].w / 2 label:k_btn[i].label];
    for (int i = 0; i < 4; i++) _dp[i] = [self ring:k_dpad_rect.size.width / 6 label:(const char *[]){"◀\uFE0E", "▲\uFE0E", "▶\uFE0E", "▼\uFE0E"}[i]];
    for (int i = 0; i < 2; i++) { _stk[i][0] = [self ring:56 label:NULL]; _stk[i][1] = [self ring:24 label:NULL]; _stk[i][0].hidden = _stk[i][1].hidden = YES; }
    _opa = [UIButton buttonWithType:UIButtonTypeSystem]; [_opa setTitle:@"◐" forState:UIControlStateNormal]; _opa.titleLabel.font = [UIFont systemFontOfSize:20];
    [_opa setTitleColor:UIColor.whiteColor forState:UIControlStateNormal]; [_opa addTarget:self action:@selector(cycleAlpha) forControlEvents:UIControlEventTouchUpInside]; [self addSubview:_opa];
    // 滑动视角：每 1/60 秒按手指这一拍移动的点数换算右摇杆（6 点 / 拍 = 推满），手指停住就回中 | swipe camera: every 1/60 s the finger's movement this tick becomes the right stick (6 points per tick = full), and it recentres when the finger stops
    __weak B3TouchView *ws = self;
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *tm) { (void)tm; B3TouchView *me = ws; if (me) [me camTick]; }];
    _mode = [self smallButton:@"menu" action:@selector(toggleMenu)]; _back = [self smallButton:@"B" action:@selector(backTap)]; _back.hidden = YES;
    for (int i = 0; i < NBTN; i++) _btn[i].hidden = k_btn[i].in_menu == 2;
    [self applyAlpha]; return self;
}
- (CAShapeLayer *)ring:(float)r label:(const char *)t {
    CAShapeLayer *l = [CAShapeLayer layer]; l.bounds = CGRectMake(0, 0, 2 * r, 2 * r);
    l.path = [UIBezierPath bezierPathWithOvalInRect:l.bounds].CGPath; l.fillColor = [UIColor colorWithWhite:0 alpha:0.35].CGColor;
    l.strokeColor = UIColor.whiteColor.CGColor; l.lineWidth = 2;
    if (t) { CATextLayer *tx = [CATextLayer layer]; tx.string = @(t); tx.fontSize = r > 26 ? 13 : 11; tx.alignmentMode = kCAAlignmentCenter; tx.foregroundColor = UIColor.whiteColor.CGColor;
             tx.contentsScale = self.traitCollection.displayScale; tx.frame = CGRectMake(0, r - tx.fontSize * 0.65, 2 * r, tx.fontSize * 1.4); [l addSublayer:tx]; }
    [self.layer addSublayer:l]; return l;
}
- (CAShapeLayer *)box:(CGSize)sz label:(const char *)t {   // 圆角矩形键 | rounded-rectangle button
    CAShapeLayer *l = [CAShapeLayer layer]; l.bounds = CGRectMake(0, 0, sz.width, sz.height);
    l.path = [UIBezierPath bezierPathWithRoundedRect:l.bounds cornerRadius:8].CGPath; l.fillColor = [UIColor colorWithWhite:0 alpha:0.35].CGColor;
    l.strokeColor = UIColor.whiteColor.CGColor; l.lineWidth = 1.5;
    CATextLayer *tx = [CATextLayer layer]; tx.string = @(t); tx.fontSize = 15; tx.alignmentMode = kCAAlignmentCenter; tx.foregroundColor = UIColor.whiteColor.CGColor;
    tx.contentsScale = self.traitCollection.displayScale; tx.frame = CGRectMake(0, sz.height / 2 - 10, sz.width, 20); [l addSublayer:tx];
    [self.layer addSublayer:l]; return l;
}
- (CGRect)scaled:(CGRect)r { CGSize s = self.bounds.size; CGFloat sx = s.width / DESIGN_W, sy = s.height / DESIGN_H; return CGRectMake(r.origin.x * sx, r.origin.y * sy, r.size.width, r.size.height); }
- (UIButton *)smallButton:(NSString *)t action:(SEL)a {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem]; [b setTitle:t forState:UIControlStateNormal]; b.titleLabel.font = [UIFont boldSystemFontOfSize:15];
    [b setTitleColor:UIColor.whiteColor forState:UIControlStateNormal]; b.backgroundColor = [UIColor colorWithWhite:0 alpha:0.4]; b.layer.cornerRadius = 8;
    b.layer.borderColor = UIColor.whiteColor.CGColor; b.layer.borderWidth = 1.5; [b addTarget:self action:a forControlEvents:UIControlEventTouchUpInside]; [self addSubview:b]; return b;
}
- (void)pulse:(int)bit {   // 按一下：按下约 80 ms 后松开 | one press: released after about 80 ms
    _pulse |= 1ull << bit; [_hap impactOccurred]; [self recompute];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 80 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ self->_pulse &= ~(1ull << bit); [self recompute]; });
}
- (void)backTap { [self pulse:1]; }
- (void)toggleMenu {
    [self clearAll]; _menu = !_menu; [UIView performWithoutAnimation:^{ [self->_mode setTitle:self->_menu ? @"👊" : @"menu" forState:UIControlStateNormal]; [self->_mode layoutIfNeeded]; }];
    _back.hidden = !_menu; [self setNeedsLayout]; [self layoutIfNeeded];   // 换标题不带动画：系统按钮默认淡入 | title change without animation: system buttons fade in by default
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBTN; i++) _btn[i].hidden = _menu ? k_btn[i].in_menu == 0 : k_btn[i].in_menu == 2; for (int i = 0; i < 4; i++) _dp[i].hidden = _menu;
    [CATransaction commit]; LOG("B3 触屏层 %s | touch layer %s", _menu ? "菜单模式" : "战斗模式", _menu ? "menu mode" : "combat mode");
}
- (void)cycleAlpha { _alpha = _alpha < 0.2f ? 0.35f : _alpha < 0.4f ? 0.6f : _alpha < 0.7f ? 0.9f : 0.15f; [NSUserDefaults.standardUserDefaults setFloat:_alpha forKey:@"b3_touch_alpha"]; [self applyAlpha]; }
- (void)applyAlpha { for (CALayer *l in self.layer.sublayers) if (![l.delegate isKindOfClass:UIView.class]) l.opacity = _alpha; _opa.alpha = MAX(_alpha, 0.5f); }
- (CGPoint)posOf:(int)i { CGRect r = [self scaled:CGRectMake(k_btn[i].x, k_btn[i].y, k_btn[i].w, k_btn[i].h)]; return CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)); }
- (CGPoint)dpadCentre { CGRect r = [self scaled:k_dpad_rect]; return CGPointMake(CGRectGetMidX(r), CGRectGetMidY(r)); }
- (void)layoutSubviews {
    [super layoutSubviews]; [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBTN; i++) _btn[i].position = [self posOf:i];
    CGPoint c = [self dpadCentre]; CGFloat o = k_dpad_rect.size.width / 3; for (int i = 0; i < 4; i++) _dp[i].position = CGPointMake(c.x + k_dpad[i].dx * o, c.y + k_dpad[i].dy * o);
    _opa.frame = [self scaled:k_alpha_btn];
    _mode.frame = [self scaled:_menu ? k_menu_btn_m : k_menu_btn];
    _back.frame = [self scaled:k_back_btn];
    [CATransaction commit];
}
- (int)hit:(CGPoint)p {   // 按钮命中（触点外扩 8 点）| button hit (touch area grown by 8 points)
    int best = -1; CGFloat bd = 1e9;
    for (int i = 0; i < NBTN; i++) { if (_btn[i].hidden) continue; CGPoint c = [self posOf:i]; CGFloat d = hypot(p.x - c.x, p.y - c.y);
        BOOL in = k_btn[i].rect ? fabs(p.x - c.x) < k_btn[i].w / 2 + 6 && fabs(p.y - c.y) < k_btn[i].h / 2 + 6 : d < k_btn[i].w / 2 + 8;
        if (in && d < bd) { bd = d; best = i; } }
    return best;
}
- (int)dpadDir:(CGPoint)p {   // 十字键方向（十字键方框内，按角度分四向）| D-pad direction (inside the D-pad box, four sectors by angle)
    CGPoint c = [self dpadCentre]; CGFloat dx = p.x - c.x, dy = p.y - c.y; if (fabs(dx) > k_dpad_rect.size.width / 2 + 6 || fabs(dy) > k_dpad_rect.size.height / 2 + 6) return -1;
    return fabs(dx) > fabs(dy) ? (dx < 0 ? 0 : 2) : (dy < 0 ? 1 : 3);
}
- (void)recompute {
    uint64_t b = (_zlLatch ? 1ull << 8 : 0) | _pulse; for (int i = 0; i < NBTN; i++) if (_owner[i]) b |= 1ull << k_btn[i].bit;
    if (_dpOwner) { int d = [self dpadDir:[_dpOwner locationInView:self]]; if (d >= 0) b |= 1ull << k_dpad[d].bit; for (int i = 0; i < 4; i++) _dp[i].fillColor = [UIColor colorWithWhite:i == d ? 1 : 0 alpha:i == d ? 0.6 : 0.35].CGColor; }
    else for (int i = 0; i < 4; i++) _dp[i].fillColor = [UIColor colorWithWhite:0 alpha:0.35].CGColor;
    int32_t v[2][2] = {{0, 0}, {0, 0}};
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int k = 0; k < 2; k++) {
        _stk[k][0].hidden = _stk[k][1].hidden = !_stkT[k] || k; if (!_stkT[k]) continue;
        if (k) { v[1][0] = (int32_t)(_camV.x * 32767); v[1][1] = (int32_t)(-_camV.y * 32767); continue; }
        CGPoint p = [_stkT[k] locationInView:self]; CGFloat dx = p.x - _stkO[k].x, dy = p.y - _stkO[k].y, m = hypot(dx, dy);
        if (m > 56) { dx *= 56 / m; dy *= 56 / m; m = 56; }
        _stk[k][0].position = _stkO[k]; _stk[k][1].position = CGPointMake(_stkO[k].x + dx, _stkO[k].y + dy);
        if (m / 56 > 0.15) { v[k][0] = (int32_t)(dx / 56 * 32767); v[k][1] = (int32_t)(-dy / 56 * 32767); }   // 屏幕 y 向下，摇杆 y 向上 | screen y points down, stick y points up
    }
    for (int i = 0; i < NBTN; i++) { BOOL on = _owner[i] || (k_btn[i].bit == 8 && _zlLatch); _btn[i].fillColor = on ? [UIColor colorWithRed:0.75 green:0.3 blue:1 alpha:0.6].CGColor : [UIColor colorWithWhite:0 alpha:0.35].CGColor; }
    [CATransaction commit];
    g_touch_btn = b; g_touch_lx = v[0][0]; g_touch_ly = v[0][1]; g_touch_rx = v[1][0]; g_touch_ry = v[1][1]; push();
}
- (void)camTick {
    // 自动切菜单 / 战斗已取消（遇敌后战斗 HUD 的界面绘制也上百，被误判成菜单）；只用手动按钮。b3_nvn.c 的识别只记日志 B3-UIMODE
    // Automatic menu / combat switching removed (after meeting enemies the combat HUD also draws over a hundred UI elements and was taken for a menu); the manual button only. The detection in b3_nvn.c only logs B3-UIMODE
    if (!_stkT[1]) { if (_camV.x || _camV.y) { _camV = CGPointZero; [self recompute]; } return; }
    CGPoint p = [_stkT[1] locationInView:self]; CGFloat dx = (p.x - _camPrev.x) / 6, dy = (p.y - _camPrev.y) / 6; _camPrev = p;
    _camV = CGPointMake(fmax(-1, fmin(1, dx)), fmax(-1, fmin(1, dy))); [self recompute];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    if (_menu) { UITouch *t = ts.anyObject; int h = [self hit:[t locationInView:self]];   // 菜单模式里看得见的键（+）照常按 | visible buttons in menu mode (+) work as usual
        if (h >= 0) { if (!_owner[h]) { _owner[h] = t; [_hap impactOccurred]; [self recompute]; } return; }
        if (!_mt) { _mt = t; _mO = [t locationInView:self]; _mT0 = t.timestamp; } return; }
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self]; int h = [self hit:p];
        if (h >= 0) { if (k_btn[h].bit == 8) { _zlLatch = !_zlLatch; [_hap impactOccurred]; LOG("B3 魔兽键 %s | demon %s", _zlLatch ? "按住 ZL" : "松开 ZL", _zlLatch ? "hold ZL" : "release ZL"); continue; }
                      if (!_owner[h]) { _owner[h] = t; [_hap impactOccurred]; } continue; }
        if (!_dpOwner && [self dpadDir:p] >= 0) { _dpOwner = t; [_hap impactOccurred]; continue; }
        // 左摇杆只在设计稿的摇杆区（k_zone）调出；其余地方按下 = 滑动视角（不显示摇杆）| the left stick appears only in the design's stick zone (k_zone); elsewhere a press = swipe camera (no stick shown)
        CGSize sz = self.bounds.size; CGRect z = [self scaled:k_zone]; z.size = CGSizeMake(k_zone.size.width * sz.width / DESIGN_W, k_zone.size.height * sz.height / DESIGN_H); int k = CGRectContainsPoint(z, p) ? 0 : 1;
        if (!_stkT[k]) { _stkT[k] = t; _stkO[k] = p; _stkT0[k] = t.timestamp; if (k) { _camPrev = p; _camV = CGPointZero; } }
    }
    [self recompute];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)ts; (void)e; if (_stkT[0]) [self recompute]; }
- (void)drop:(NSSet<UITouch *> *)ts {
    for (UITouch *t in ts) for (int i = 0; i < NBTN; i++) if (_owner[i] == t) { _owner[i] = nil; [self recompute]; }
    if (_mt && [ts containsObject:_mt]) {   // 菜单模式：轻点 = A，滑动 ≥ 30 点 = 十字键一格（按主方向）| menu mode: tap = A, swipe ≥ 30 points = one D-pad step (dominant direction)
        CGPoint p = [_mt locationInView:self]; CGFloat dx = p.x - _mO.x, dy = p.y - _mO.y; _mt = nil;
        if (hypot(dx, dy) >= 30) [self pulse:fabs(dx) > fabs(dy) ? (dx < 0 ? 12 : 14) : (dy < 0 ? 13 : 15)];
        else if (hypot(dx, dy) < 12) [self pulse:0];
        return;
    }
    for (UITouch *t in ts) { for (int i = 0; i < NBTN; i++) if (_owner[i] == t) _owner[i] = nil; if (_dpOwner == t) _dpOwner = nil; for (int k = 0; k < 2; k++) if (_stkT[k] == t) _stkT[k] = nil; }
    [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self drop:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self drop:ts]; }
- (void)clearAll { memset(_owner, 0, sizeof _owner); _zlLatch = NO; _mt = nil; _pulse = 0; _dpOwner = nil; _stkT[0] = _stkT[1] = nil; [self recompute]; }
@end

@interface B3MetalView : UIView @end
@implementation B3MetalView
+ (Class)layerClass { return [CAMetalLayer class]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat s = self.window.windowScene.screen.nativeScale ?: self.traitCollection.displayScale;
    ((CAMetalLayer *)self.layer).drawableSize = CGSizeMake(self.bounds.size.width * s, self.bounds.size.height * s);
}
@end

@interface B3ViewController : UIViewController @end
@implementation B3ViewController
- (void)loadView { self.view = [B3MetalView new]; self.view.backgroundColor = UIColor.blackColor; }
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

@interface B3SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation B3SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opts {
    (void)session; (void)opts;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    B3ViewController *vc = [B3ViewController new]; self.window.rootViewController = vc; [self.window makeKeyAndVisible];
    g_layer = (CAMetalLayer *)vc.view.layer;
    g_layer.device = MTLCreateSystemDefaultDevice(); g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm; g_layer.opaque = YES;
    [vc.view setNeedsLayout]; [vc.view layoutIfNeeded];
    // 触屏层盖在画面上；有手柄连着就藏起来（连上 / 断开时切换）| touch layer over the picture; hidden while a gamepad is connected (toggled on connect / disconnect)
    static B3TouchView *tv; tv = [[B3TouchView alloc] initWithFrame:vc.view.bounds]; tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; [vc.view addSubview:tv];
    void (^vis)(void) = ^{ BOOL pad = GCController.controllers.count > 0; if (pad && !tv.hidden) [tv clearAll]; tv.hidden = pad; LOG("B3 触屏层%s | touch layer %s", pad ? "隐藏（有手柄）" : "显示", pad ? "hidden (gamepad)" : "shown"); };
    vis();
    if (getenv("B3_TOUCH_SHOT")) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{   // B3_TOUCH_SHOT=1 / menu：5 秒后把触屏层画成 Documents/touch.png（核对布局）| B3_TOUCH_SHOT=1 / menu: after 5 s render the touch layer to Documents/touch.png (layout check)
        BOOL h = tv.hidden; tv.hidden = NO; if (!strcmp(getenv("B3_TOUCH_SHOT"), "menu") && ![[tv valueForKey:@"_menu"] boolValue]) [tv performSelector:@selector(toggleMenu)];   // =menu：截菜单模式 | =menu: snapshot menu mode
        UIGraphicsImageRenderer *r = [[UIGraphicsImageRenderer alloc] initWithBounds:tv.bounds];
        NSData *png = [r PNGDataWithActions:^(UIGraphicsImageRendererContext *c) { [UIColor.darkGrayColor setFill]; [c fillRect:tv.bounds]; [tv.layer renderInContext:c.CGContext]; }];
        tv.hidden = h; [png writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/touch.png"] atomically:YES]; LOG("B3 触屏层截图已存 | touch layer snapshot saved"); });
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; vis(); }];
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // 手柄玩时屏幕不自动熄 | the screen does not auto-lock while playing with a gamepad
    void (^hook)(GCController *) = ^(GCController *c) { c.extendedGamepad.buttonHome.preferredSystemGestureState = GCSystemGestureStateDisabled;
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gp, GCControllerElement *el) { (void)gp; (void)el; push(); }; LOG("B3 手柄已连 | gamepad connected: %s", c.vendorName.UTF8String ?: "?"); };
    for (GCController *c in GCController.controllers) hook(c);
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { hook(n.object); vis(); }];
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 120 repeats:YES block:^(NSTimer *t) { (void)t; if (GCController.controllers.count) push(); }];
    // 热状态每 30 秒记一行（性能基准看发热）| thermal state logged every 30 s (benchmarks track heat)
    [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES block:^(NSTimer *t) { (void)t; static const char *k[] = {"nominal", "fair", "serious", "critical"};
        NSProcessInfoThermalState ts = NSProcessInfo.processInfo.thermalState; LOG("B3-THERMAL %s", ts <= 3 ? k[ts] : "?"); }];   // 120 Hz 轮询（短按整形的时间点也靠它）| 120 Hz polling (also drives the tap-shaping time points)
    LOG("B3 iOS 窗口已开 %.0fx%.0f 点 | iOS window open %.0fx%.0f points", vc.view.bounds.size.width, vc.view.bounds.size.height, vc.view.bounds.size.width, vc.view.bounds.size.height);
    static int started; if (started++) return;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_tramp, (void *)g_guest)) die("B3 客体线程创建失败 | guest thread creation failed");
}
@end

@interface B3AppDelegate : UIResponder <UIApplicationDelegate> @end
@implementation B3AppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    (void)app; (void)o; UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role]; c.delegateClass = [B3SceneDelegate class]; return c;
}
@end

void b3_front_run(void (*guest)(void)) {
    g_guest = guest;
    @autoreleasepool { static char *av[] = {"B3", NULL}; UIApplicationMain(1, av, nil, NSStringFromClass([B3AppDelegate class])); }
}
