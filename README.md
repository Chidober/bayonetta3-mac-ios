# B3 Port: Bayonetta 3 on Mac / iPhone

English · [中文](README.zh-CN.md)

Turns your own copy of the Switch game *Bayonetta 3* into a program that runs natively on Apple Silicon Macs and iPhones.

- Not an emulator: the game's ARM64 code runs directly on the Apple chip, system services are implemented by this project on the host, and graphics calls (NVN) are translated to Metal.
- **This repository contains no game files, shaders, keys or saves.** You need a legitimate copy of the game and must dump the files from your own console.
- All generated files live in a work directory outside the repository (default `~/b3-work`).

---

## 1. Requirements

| Item | Requirement |
|---|---|
| Computer | Apple Silicon (M-series) Mac, macOS 26 or later |
| Developer tools | Xcode 26 or later (with the Metal toolchain), [Homebrew](https://brew.sh) |
| Phone (optional) | iPhone, tested on iOS 26 or later, **8 GB of memory(better)** (tested on iPhone 15 Pro and Duo); a cable |
| Apple account (for the phone) | A free Apple ID is enough (signing lasts 7 days; reinstall when it expires) |
| Game | *Bayonetta 3* for Switch, cartridge or digital, **version 1.0.0 with no update** (Title ID `01004A4010FEA000`) |
| Disk space | About 20 GB (14 GB of game data + shader cache and build files) |
| Controls | Mac: keyboard or gamepad; iPhone: touch or Bluetooth gamepad |

## 2. Dump the game files

Using your own Switch and game, dump the files with [nxdumptool](https://github.com/DarkMatterCore/nxdumptool) (this project neither provides keys nor explains how to obtain them):

1. In nxdumptool, select the game → **NCA FS section dump** → **Program #0**;
2. Dump **section 0 (ExeFS)** and **section 1 (RomFS)**;
3. On the computer, arrange them into one folder like this:

```
your-folder/
├── ExeFS/
│   ├── main
│   ├── main.npdm
│   ├── rtld
│   ├── sdk
│   └── subsdk0
└── RomFS/
    └── … (about 530 files, 14 GB in total)
```

## 3. One-click play

**Mac:** double-click **`Play B3.command`** in the repository. The first time, a dialog asks for the game folder from step 2; everything else then happens automatically:

1. installs dependencies (dotnet / spirv-cross / xcodegen from Homebrew, and a Python environment);
2. rewrites the game's code modules;
3. starts the game once in the background to collect all shaders (about 1–2 minutes, no window);
4. compiles the shaders into a Metal cache (**slow only the first time: about 40 minutes to 1.5 hours depending on the machine**; instant afterwards);
5. opens the game window.

You can also run it from a terminal with the same effect:

```bash
./b3 play /path/to/your-game-folder
```

After that, just double-click `Play B3.command` or run `./b3 play` to play.

**iPhone:** connect an unlocked iPhone with Developer Mode turned on (see section 5) by cable, make sure you are signed in to an Apple ID in Xcode, then run:

```bash
./b3 ios
```

It fills in any missing preparation steps, builds the iPhone shader cache (also slow the first time), finds the signing team in Xcode and the connected phone, then packages and installs the app and pushes the game data.

Sections 4 and 5 below describe each step separately, useful for troubleshooting.

## 4. Step by step (Mac)

In a terminal, go to this repository's folder and run in order:

```bash
./b3 setup
```

Checks Xcode, installs dotnet / spirv-cross / xcodegen (through Homebrew) and creates a Python environment.

```bash
./b3 import /path/to/your-folder
```

Records where the game files are (referenced read-only, not copied). Warns if the version is not 1.0.0.

```bash
./b3 boot
```

Builds the host program and rewrites the game's three code modules so they can run on Mac / iPhone (about 1 minute).

```bash
./b3 collect
```

Starts the game once in the background and collects every shader the game registers at startup (about 1–2 minutes, no window).

```bash
./b3 shaders
```

Translates the collected shaders and compiles them into a Metal cache. **The first run takes a while** (about 6,300 shaders, 40 minutes to 1.5 hours); later runs only compile what is new.

```bash
./b3 play
```

Play. Saves are in `~/b3-work/save`, logs in `~/b3-work/logs`. Run `./b3 status` at any time to see progress.

### Mac controls

Connect a gamepad (Xbox / PlayStation / Switch Pro, etc.) and it works directly; buttons map by position to the Switch layout (bottom = B, right = A, left = Y, top = X), and the gamepad's Home button toggles the HOME menu.

Keyboard:

| Switch | Keyboard |
|---|---|
| A / B | L / K (Return = A, Backspace = B) |
| X / Y | I / J |
| L / R | Q / E |
| ZL / ZR | Z / Space |
| + / − | = / - |
| L3 / R3 | U / O |
| D-pad | Arrow keys |
| Left stick | W A S D |
| Right stick | G (up) V (left) B (down) N (right) |
| Home | H |
| Full screen on / off | F11 |
| Frame grab (debugging) | F12 |

### Mac display

- The game runs in docked mode and renders at **1600×900**; the picture is upscaled to the window / screen size with MetalFX (e.g. 1080p in full screen). The window opens near 1600×900; **F11** toggles full screen.
- Environment variables for one launch: `B3_DOCKED=0` handheld mode (1280×720), `B3_MFX=0` plain scaling instead of MetalFX, `B3_FULLSCREEN=1` start in full screen.

### Benchmark and diagnostics

- `./b3 bench record` plays a segment and records the input (a copy of the save is used); `./b3 bench` replays it on the Mac and `./b3 bench --ios` on the connected iPhone, then prints frame time, GPU time, long frames and thermal state.
- Logs are kept small by default; `B3_DIAG_LOG=1` turns on detailed per-call logs when investigating a problem.

## 5. Install on iPhone (step by step)

1. Build the iPhone shader cache first (also slow the first time):

   ```bash
   ./b3 shaders --ios
   ```

2. Prepare signing and the device:
   - Open Xcode → Settings → Accounts, sign in with your Apple ID and note the **Team ID** (10 letters and digits);
   - Connect the iPhone by cable, unlock it and tap "Trust This Computer" on the phone;
   - On the phone open **Settings → Privacy & Security → Developer Mode** and restart when prompted;
   - Find the device UDID:

     ```bash
     xcrun devicectl list devices
     ```

3. Package, install and push the data (about 15 GB, a few minutes over a cable). With only one phone connected and one account signed in to Xcode, the options can be omitted:

   ```bash
   ./b3 ios --team <your Team ID> --device <device UDID>
   ```

   Optionally set the app's bundle ID with `--bundle com.yourname.b3` (default `com.b3port.<team id>`).

4. Before opening the app for the first time, trust your developer account on the phone under **Settings → General → VPN & Device Management**.

Free-account signing expires after 7 days: just run `./b3 ios` again (game data already on the phone is not pushed again).

### iPhone controls

- **Bluetooth gamepad:** the touch buttons hide automatically once connected; controls are the same as on the Mac.
- **Touch (combat mode):** pressing in the lower-left area brings up the movement stick; swiping anywhere else turns the camera; action buttons on the right are 👊 X, 🔫 Y, 🦶 A, 🦋 B, 💨 ZR, etc.; 🌛 ZL toggles — one tap holds it, another tap releases it (demon summon); −, + and the opacity button ◐ are at the top.
- **Touch (menu mode):** tap **menu** at the top to switch. A tap = confirm (A), swipes up / down / left / right = D-pad, plus B (back), L / R / ZL / ZR and +; tap 👊 to return to combat mode.
- The game itself does not support touch, so the menu "taps" are converted into button presses.

## 6. Known issues

- **Occasional minor graphics glitches early on, improving as you play.** Some shaders compile most accurately only when the vertex and texture formats the game actually uses at run time are known; that information can only be gathered from the logs while you play and cannot ship with the repository. This is handled automatically: after every 3 sessions, the next launch does an incremental shader cache rebuild with the new information (about 2 minutes, announced in the terminal); the iPhone build updates the next time you run `./b3 ios`. If you see an obvious graphics error, play a few more sessions and check again, or run `./b3 shaders` (add `--ios` for iPhone) to update right away.
- 13 tessellation shaders in the game cannot be translated yet (Metal has no equivalent stages), so the few objects that use them are not drawn.
- The first time a new scene appears, render pipelines are created on the spot and cause a brief stutter. They are recorded, and from the next launch on they are built in the background at startup, so the same scenes no longer stutter.
- On iPhone (15 Pro) most scenes run at 60 fps; heavy scenes, or a phone that is already hot, drop to about 30–40 fps. The game pauses while the app is in the background.
- With Bluetooth headphones the sound lags the picture by about 0.15 s (the headphones' own latency).

## 7. Directory layout

```
b3                     command-line entry point (./b3 help lists every command)
Play B3.command        double-click for one-click play on the Mac
dm/b3/                 this game's system services, input, sound, graphics hookup, Mac / iOS front ends
dm/b3/shader/          shader translation (Switch → SPIR-V → Metal)
dm/b3/ios/             iOS app entry point and packaging script
dm/host/               generic runtime: module loading, system calls, threads
dm/gfx/                generic graphics layer: NVN → Metal, texture decoding
dm/tools/patch_nso.py  game code module rewriter
dm/third_party/        third-party sources (Ryujinx shader translation library, bcdec)
```

## 8. Acknowledgements

- [D2R Mobile](https://github.com/jhaizhou-ops/d2r-mobile) (by jhaizhou, MIT): this project's generic runtime — module loading, system calls, the NVN → Metal graphics layer and texture decoding — comes from it.
- [Ryujinx](https://github.com/Ryubing) (MIT): shader translation library.
- [bcdec](https://github.com/iOrange/bcdec) (MIT / public domain): BC texture decoding.
- [switchbrew](https://switchbrew.org) and [libnx](https://github.com/switchbrew/libnx): public documentation of system services and data structures.
- [nxdumptool](https://github.com/DarkMatterCore/nxdumptool): dumping your own game files.

## 9. License

MIT, see [LICENSE](LICENSE). Third-party code keeps its own licenses (`dm/third_party/*`).

This project is not affiliated with Nintendo or PlatinumGames. *Bayonetta 3* and related trademarks belong to their respective owners. Only use a copy of the game that you legally own.
