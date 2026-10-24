# B3 Port：《猎天使魔女 3》在 Mac / iPhone 上运行

把你自己拥有的 Switch 版《猎天使魔女 3》（Bayonetta 3）游戏文件，转换成可以在 Apple Silicon Mac 和 iPhone 上原生运行的程序。

- 不是模拟器：游戏的 ARM64 代码直接在 Apple 芯片上执行，系统服务由本项目在宿主上实现，图形调用（NVN）翻译成 Metal。
- **本仓库不包含任何游戏文件、着色器、密钥或存档。** 你需要有正版游戏，并自己从主机导出文件。
- 所有生成物都放在仓库外的工作目录（缺省 `~/b3-work`）。

> English summary: this repository turns *your own* dump of Bayonetta 3 (Switch, v1.0.0) into a native macOS / iOS program. No game data is included. Mac: double-click `Play B3.command` (or run `./b3 play <game dir>`); iPhone: `./b3 ios`. Every preparation step runs automatically the first time.

---

## 1. 需要准备什么

| 项目 | 要求 |
|---|---|
| 电脑 | Apple Silicon（M 系列）Mac，macOS 26 或更新 |
| 开发工具 | Xcode 26 或更新（含 Metal 工具链），[Homebrew](https://brew.sh) |
| 手机（可选） | iPhone，iOS 26 或更新，**8 GB 内存**（iPhone 15 Pro 及更新机型）；数据线 |
| Apple 账号（手机用） | 免费的 Apple ID 即可（签名 7 天有效，过期重装一次） |
| 游戏 | 《猎天使魔女 3》Switch 卡带 / 数字版，**版本 1.0.0，不装更新**（Title ID `01004A4010FEA000`） |
| 磁盘空间 | 约 20 GB（游戏数据 14 GB + 着色器缓存与构建） |
| 操作 | Mac：键盘或手柄；iPhone：触屏或蓝牙手柄 |

## 2. 导出游戏文件

用你自己的 Switch 和游戏，通过 [nxdumptool](https://github.com/DarkMatterCore/nxdumptool) 导出（本项目不提供也不讲解获取密钥的方法）：

1. 在 nxdumptool 里选择游戏 → **NCA FS section dump** → **Program #0**；
2. 导出 **section 0（ExeFS）** 和 **section 1（RomFS）**；
3. 在电脑上整理成这样一个目录：

```
你的目录/
├── ExeFS/
│   ├── main
│   ├── main.npdm
│   ├── rtld
│   ├── sdk
│   └── subsdk0
└── RomFS/
    └── …（约 530 个文件，共 14 GB）
```

## 3. 一键游玩

**Mac：** 双击仓库里的 **`Play B3.command`**，第一次会弹窗让你选择第 2 步整理好的游戏目录，之后全部自动完成：

1. 安装依赖（Homebrew 的 dotnet / spirv-cross / xcodegen 和一个 Python 环境）；
2. 改写游戏代码模块；
3. 后台启动一次游戏，收集全部着色器（约 1–2 分钟，不开窗口）；
4. 把着色器编成 Metal 缓存（**只有第一次很慢，约 40 分钟到 1.5 小时，视机器而定**，之后秒开）；
5. 打开游戏窗口。

也可以在终端里运行，效果相同：

```bash
./b3 play /path/to/你的游戏目录
```

以后再玩只需双击 `Play B3.command` 或运行 `./b3 play`。

**iPhone：** 用数据线连上已解锁、已打开开发者模式的 iPhone（见第 5 节），在 Xcode 里登录过 Apple ID，然后运行：

```bash
./b3 ios
```

它会自动补齐上面的准备步骤、生成手机版着色器缓存（第一次同样较慢）、找到 Xcode 里的签名团队和连着的手机、打包安装并推送游戏数据。

下面第 4、5 节是分步说明，出问题时可以对照排查。

## 4. 分步运行（Mac）

在终端里进入本仓库目录，依次执行：

```bash
./b3 setup
```

检查 Xcode、安装 dotnet / spirv-cross / xcodegen（通过 Homebrew），并建一个 Python 环境。

```bash
./b3 import /path/to/你的目录
```

登记游戏文件位置（只读引用，不复制）。如果版本不是 1.0.0 会给出警告。

```bash
./b3 boot
```

编译宿主程序，并把游戏的三个代码模块改写成能在 Mac / iPhone 上执行的形式（约 1 分钟）。

```bash
./b3 collect
```

后台启动一次游戏，收集游戏启动时登记的全部着色器（约 1–2 分钟，不开窗口）。

```bash
./b3 shaders
```

把收集到的着色器翻译、编译成 Metal 缓存。**第一次需要较长时间**（约 6300 个着色器，约 40 分钟到 1.5 小时）；以后只编新增的。

```bash
./b3 play
```

开始玩。存档在 `~/b3-work/save`，日志在 `~/b3-work/logs`。随时可用 `./b3 status` 查看进度。

### Mac 操作

接上手柄（Xbox / PS / Switch Pro 等）即可直接用，按键按位置对应 Switch（下 = B、右 = A、左 = Y、上 = X），手柄的 Home 键切换主页。

键盘：

| Switch | 键盘 |
|---|---|
| A / B | L / K（回车 = A，退格 = B） |
| X / Y | I / J |
| L / R | Q / E |
| ZL / ZR | Z / 空格 |
| + / − | = / - |
| L3 / R3 | U / O |
| 十字键 | 方向键 |
| 左摇杆 | W A S D |
| 右摇杆 | G（上）V（左）B（下）N（右） |
| Home | H |
| 抓帧（调试） | F12 |

## 5. 装到 iPhone（分步）

1. 先生成手机版着色器缓存（同样第一次较慢）：

   ```bash
   ./b3 shaders --ios
   ```

2. 准备签名与设备：
   - 打开 Xcode → Settings → Accounts，登录你的 Apple ID，记下 **Team ID**（10 位字母数字）；
   - 用数据线连接 iPhone，解锁并在手机上“信任这台电脑”；
   - 手机上打开 **设置 → 隐私与安全性 → 开发者模式**，按提示重启；
   - 查设备 UDID：

     ```bash
     xcrun devicectl list devices
     ```

3. 打包、安装并推送数据（约 15 GB，数据线几分钟）。只连着一台手机、Xcode 只登录一个账号时可以省略参数：

   ```bash
   ./b3 ios --team <你的 Team ID> --device <设备 UDID>
   ```

   可选 `--bundle com.yourname.b3` 指定应用包名（缺省 `com.b3port.<团队号>`）。

4. 第一次打开前，在手机上 **设置 → 通用 → VPN与设备管理** 里信任你的开发者账号。

免费账号的签名 7 天后过期：到时重新运行一次 `./b3 ios`（手机上已有的游戏数据不会重推）。

### iPhone 操作

- **蓝牙手柄**：连上后触屏按键自动隐藏，操作同 Mac。
- **触屏（战斗模式）**：左下角区域按下出现移动摇杆；其他空白处滑动转视角；右侧为 👊 X、🔫 Y、🦶 A、🦋 B、💨 ZR 等动作键；🌛 ZL 点一下按住、再点一下松开（魔兽召唤）；顶部有 −、+、透明度 ◐。
- **触屏（菜单模式）**：点顶部 **menu** 切换。轻点 = 确认（A），上下左右滑动 = 方向键，另有 B（返回）、L / R / ZL / ZR、+；点 👊 回到战斗模式。
- 游戏本身不支持触屏点击，菜单里的“轻点”是转成按键实现的。

## 6. 已知问题

- **刚开始玩时可能偶有画面小问题，会越玩越准。** 一部分着色器要知道游戏运行时实际用的顶点格式、贴图格式才能编得最准，这些信息只能在你玩的过程中从日志里积累，无法随仓库提供。程序会自动处理：每累计玩 3 局，下次启动时用新信息增量重编一次着色器缓存（约 2 分钟，终端里会提示）；手机版在下次运行 `./b3 ios` 时同样更新。遇到明显的画面错误，多玩几局后再看；也可以手动运行 `./b3 shaders`（手机版加 `--ios`）立即更新。
- 游戏里有 13 个曲面细分着色器暂时无法翻译（Metal 没有对应阶段），用到它们的少量物体不会显示。
- 第一次进入新场景时要现场创建渲染管线，会卡顿一下，之后同一场景不再卡。
- iPhone 上重场景帧率约 20–40 帧，发热与大型游戏相近。
- 使用蓝牙耳机时声音会比画面晚约 0.15 秒（耳机本身的延迟）。

## 7. 目录结构

```
b3                     命令行入口（./b3 help 查看全部命令）
Play B3.command        双击一键在 Mac 上游玩
dm/b3/                 本游戏的系统服务、输入、声音、图形接入，Mac / iOS 前端
dm/b3/shader/          着色器翻译（Switch → SPIR-V → Metal）
dm/b3/ios/             iOS 应用入口与打包脚本
dm/host/               通用运行时：模块装载、系统调用、线程
dm/gfx/                通用图形层：NVN → Metal、纹理解码
dm/tools/patch_nso.py  游戏代码模块改写器
dm/third_party/        第三方源码（Ryujinx 着色器翻译库、bcdec）
```

## 8. 致谢

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile)（作者 jhaizhou，MIT）：本项目的通用运行时——模块装载、系统调用、NVN→Metal 图形层与纹理解码——来自这个项目。
- [Ryujinx](https://github.com/Ryubing)（MIT）：着色器翻译库。
- [bcdec](https://github.com/iOrange/bcdec)（MIT / 公有领域）：BC 纹理解码。
- [switchbrew](https://switchbrew.org) 与 [libnx](https://github.com/switchbrew/libnx)：系统服务与数据结构的公开文档。
- [nxdumptool](https://github.com/DarkMatterCore/nxdumptool)：导出自己的游戏文件。

## 9. 许可

MIT，见 [LICENSE](LICENSE)。第三方代码保留各自的许可证（`dm/third_party/*`）。

本项目与任天堂、白金工作室无关。《猎天使魔女 3》及相关商标归其各自所有者。请只使用你合法拥有的游戏副本。
