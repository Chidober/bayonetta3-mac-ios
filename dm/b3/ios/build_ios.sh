#!/bin/zsh
# B3 iOS App 构建：编 iOS 静态库 → 把改写后的模块嵌进汇编（dm/ios/tools/gen_embed.py）→ xcodegen 生成工程 → xcodebuild 签名出包。
# 生成物全在仓库外的 $B3_IOS_WORK（缺省 ~/b3-work/b3/ios）；签名团队由 B3_TEAM 给，包名前缀由 B3_BUNDLE 给，都不进仓库。
# 用法：B3_TEAM=<团队> B3_BOOT=<模块目录> dm/b3/ios/build_ios.sh [install <设备 UDID>]
# B3 iOS App build : build the iOS static library → embed the rewritten modules into assembly (dm/ios/tools/gen_embed.py) → generate the project with xcodegen → sign and package with xcodebuild.
# All outputs live outside the repository in $B3_IOS_WORK (default ~/b3-work/b3/ios); the signing team comes from B3_TEAM and the bundle prefix from B3_BUNDLE, neither is committed.
# Usage: B3_TEAM=<team> B3_BOOT=<module dir> dm/b3/ios/build_ios.sh [install <device UDID>]
set -e
HERE=${0:A:h}; B3=${HERE:h}
: ${B3_TEAM:?需要签名团队 B3_TEAM | signing team B3_TEAM required}; : ${B3_BOOT:?需要模块目录 B3_BOOT | module directory B3_BOOT required}
B3_BUNDLE=${B3_BUNDLE:-com.example.b3}; W=${B3_IOS_WORK:-$HOME/b3-work/b3/ios}; MODS=(main subsdk0 sdk)
mkdir -p $W/Gen/boot
make -C $B3 -j10 build/ios/libb3.a >/dev/null
F=$(python3 $B3/../ios/tools/gen_embed.py $B3_BOOT $W/Gen/embed.S $MODS)
for m in $MODS; do cp $B3_BOOT/$m.fixups.tsv $B3_BOOT/$m.gates.tsv $W/Gen/boot/; done
cat > $W/Gen/b3.entitlements <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.developer.kernel.increased-memory-limit</key><true/></dict></plist>
PL
FW=(AudioToolbox AVFoundation MetalFX GameController VideoToolbox CoreMedia CoreVideo Foundation UIKit Metal QuartzCore CoreGraphics ImageIO UniformTypeIdentifiers)
LD="-Wl,-force_load,$B3/build/ios/libb3.a $F"; for f in $FW; do LD="$LD -framework $f"; done
cat > $W/project.yml <<YML
name: B3IOS
options: {bundleIdPrefix: $B3_BUNDLE, deploymentTarget: {iOS: "26.0"}}
targets:
  B3IOS:
    type: application
    platform: iOS
    sources:
      - path: $HERE/main.m
      - path: Gen/embed.S
      - path: Gen/boot
        type: folder
        buildPhase: resources
    info:
      path: Gen/Info.plist
      properties:
        CFBundleDisplayName: B3
        UIFileSharingEnabled: true
        LSSupportsOpeningDocumentsInPlace: true
        UILaunchScreen: {}
        UIStatusBarHidden: true
        UISupportedInterfaceOrientations: [UIInterfaceOrientationLandscapeRight, UIInterfaceOrientationLandscapeLeft]
        LSApplicationCategoryType: public.app-category.action-games
        LSSupportsGameMode: true
        CADisableMinimumFrameDurationOnPhone: true
        GCSupportsControllerUserInteraction: true
        GCSupportedGameControllers: [{ProfileName: ExtendedGamepad}]
        UIApplicationSceneManifest: {UIApplicationSupportsMultipleScenes: false}
    settings:
      base:
        PRODUCT_BUNDLE_IDENTIFIER: $B3_BUNDLE
        DEVELOPMENT_TEAM: $B3_TEAM
        CODE_SIGN_STYLE: Automatic
        CODE_SIGN_ENTITLEMENTS: Gen/b3.entitlements
        TARGETED_DEVICE_FAMILY: "1,2"
        OTHER_LDFLAGS: "$LD"
        CLANG_ENABLE_OBJC_ARC: YES
        DEAD_CODE_STRIPPING: NO
        ENABLE_BITCODE: NO
YML
(cd $W && xcodegen generate -q)
# 装机时按设备构建：Xcode 自动签名会把新设备登记进描述文件（免费团队的描述文件只含登记过的设备）| when installing, build for the device: Xcode automatic signing registers a new device into the profile (a free team's profile only covers registered devices)
DEST=(); [ "$1" = install ] && DEST=(-destination "id=$2")
xcodebuild -project $W/B3IOS.xcodeproj -scheme B3IOS -configuration Release -sdk iphoneos $DEST -derivedDataPath $W/dd -allowProvisioningUpdates -allowProvisioningDeviceRegistration -quiet build
APP=$W/dd/Build/Products/Release-iphoneos/B3IOS.app; echo "APP $APP"
[ "$1" = install ] && xcrun devicectl device install app --device ${2:?设备 UDID | device UDID} $APP
exit 0
