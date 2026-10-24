# B3 宿主 b3host：通用运行时（dm/host 装载与系统调用、dm/gfx NVN→Metal 图形层）加上 B3 自己的服务层（dm/b3）。
# 目标：build/b3host（macOS 可执行文件）、build/ios/libb3.a（iOS 静态库，ios/build_ios.sh 用）。
# B3 host b3host: the generic runtime (dm/host loading and system calls, dm/gfx NVN→Metal graphics layer) plus B3's own service layer (dm/b3).
# Targets: build/b3host (macOS executable), build/ios/libb3.a (iOS static library, used by ios/build_ios.sh).
MACOS_MIN ?= 26.0
# -DDM_B3=1：打开共享文件里的 B3 钩子
# -DDM_B3=1: turns on the B3 hooks in the shared files 
# B3_SHADER_CHAIN：B3 着色器库的翻译链版本号，取自 shader/CHAIN 第一词（运行时只认这个版本的库）| B3 shader library chain tag, the first word of shader/CHAIN (the runtime only accepts libraries of this tag)
B3_SHADER_CHAIN := $(firstword $(shell cat shader/CHAIN))
CFLAGS=-O1 -g -Wall -Wno-unused-function -mmacosx-version-min=$(MACOS_MIN) -DDM_B3=1 -DB3_SHADER_CHAIN=\"$(B3_SHADER_CHAIN)\"
# SDK：取 Xcode 自带的 macOS SDK（xcrun --sdk macosx），不用不带 --sdk 时可能落到的 CommandLineTools SDK（它可能比当前 ld 新、链接报 tapi 错）；不写死路径。
# 不沿用环境里的 SDKROOT：系统的 /usr/bin/python3 垫片会给子进程设成 CommandLineTools SDK（实测），所以只认 make B3_SDKROOT=… 显式覆盖
# SDK: the macOS SDK of the selected Xcode (xcrun --sdk macosx), not the CommandLineTools SDK that a bare xcrun may pick (it can be newer than the current ld and fail linking with a tapi error); no hard-coded path.
# An inherited SDKROOT is ignored: the system /usr/bin/python3 shim sets it to the CommandLineTools SDK for its children , so only an explicit make B3_SDKROOT=… overrides
B3_SDKROOT ?= $(shell env -u SDKROOT xcrun --sdk macosx --show-sdk-path)
SDKROOT := $(B3_SDKROOT)
export SDKROOT
HOST=../host
# 直接编进的通用层通用源码（一行一个，按字母序）| 通用层 generic sources compiled directly (one per line, alphabetical)
SHARED=\
	$(HOST)/crash.c \
	$(HOST)/entry.S \
	$(HOST)/hle.c \
	$(HOST)/hle_count.c \
	$(HOST)/hle_guestthread.c \
	$(HOST)/hle_os.c \
	$(HOST)/hle_svc.c \
	$(HOST)/hle_table.c \
	$(HOST)/hle_thread.c \
	$(HOST)/loader.c \
	$(HOST)/memstat.m \
	$(HOST)/thread.c
B3SRC=main_b3.c b3_front.c b3_ipc.c b3_applet.c b3_libapplet.c b3_kernel.c b3_hid.c b3_romfs.c b3_graphics.c b3_nvhost.c b3_host1x.c b3_audio.c b3_save.c b3_nvn.c b3_movie.c b3_sound_mac.c b3_front_mac.m
#通用层的 NVN→Metal 层：dm/gfx/nvn 与 dm/gfx/tex 整块编进；通用层的 nn::vi / nn::nv 强制接管（gfx/hle_vi.c、hle_nv.c）不编进，B3 在 IPC 层应答
# the base runtime's NVN→Metal layer : dm/gfx/nvn and dm/gfx/tex are compiled in whole; the base runtime's nn::vi / nn::nv forced takeovers (gfx/hle_vi.c, hle_nv.c) are not, B3 answers at the IPC layer
GFX=$(sort $(wildcard ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/include/*.h ../gfx/tex/*.h))
STUBS=stubs_host.c
FRAMEWORKS=-framework AudioToolbox -framework CoreAudio -framework GameController -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework Foundation -framework AppKit -framework Metal -framework QuartzCore -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers
CC_LINE=clang $(CFLAGS) -I. -I$(HOST) -I../gfx/nvn/include -fobjc-arc
.DEFAULT_GOAL := build/b3host
build/b3host: $(SHARED) $(B3SRC) $(GFX) $(GFX_H) $(STUBS) b3_ipc.h $(HOST)/dm.h $(HOST)/hle.h
	@mkdir -p build
	$(CC_LINE) -o $@ $(SHARED) $(B3SRC) $(GFX) $(STUBS) $(FRAMEWORKS)
# iOS 版静态库（ios/build_ios.sh 用）：同一批源码按 arm64-apple-ios 编译；前端换成 b3_front_ios.m，main_b3.c 的 main 改名 b3host_main 由 App 的 main.m 调
# iOS static library (used by ios/build_ios.sh): the same sources compiled for arm64-apple-ios; the front end is b3_front_ios.m instead, and main in main_b3.c is renamed b3host_main, called by the App's main.m
IOS_MIN ?= 26.0
IOS_SDK = $(shell env -u SDKROOT xcrun --sdk iphoneos --show-sdk-path)
IOS_SRC=$(SHARED) $(filter-out main_b3.c b3_front_mac.m,$(B3SRC)) b3_front_ios.m $(GFX) $(STUBS)
IOS_CC=env -u SDKROOT clang -target arm64-apple-ios$(IOS_MIN) -isysroot $(IOS_SDK) -O1 -g -Wall -Wno-unused-function -DDM_B3=1 -DB3_SHADER_CHAIN=\"$(B3_SHADER_CHAIN)\" -I. -I$(HOST) -I../gfx/nvn/include -fobjc-arc
IOS_OBJ=$(addprefix build/ios/,$(addsuffix .o,$(notdir $(IOS_SRC)))) build/ios/main_b3.c.o
build/ios/%.c.o: $(HOST)/%.c ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.m.o: $(HOST)/%.m ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.S.o: $(HOST)/%.S ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.c.o: ../gfx/nvn/%.c ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.m.o: ../gfx/nvn/%.m ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.S.o: ../gfx/nvn/%.S ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.c.o: ../gfx/tex/%.c ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.m.o: ../gfx/tex/%.m ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/main_b3.c.o: main_b3.c ; @mkdir -p build/ios; $(IOS_CC) -Dmain=b3host_main -c $< -o $@
build/ios/%.c.o: %.c ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
build/ios/%.m.o: %.m ; @mkdir -p build/ios; $(IOS_CC) -c $< -o $@
$(IOS_OBJ): $(GFX_H) b3_ipc.h $(HOST)/dm.h $(HOST)/hle.h
build/ios/libb3.a: $(IOS_OBJ)
	rm -f $@; libtool -static -o $@ $(IOS_OBJ)
clean:
	rm -rf build
.PHONY: clean
