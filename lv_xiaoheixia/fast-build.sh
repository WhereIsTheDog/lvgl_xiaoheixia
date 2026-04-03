#!/bin/bash
# 快速编译部署脚本 - 在 WSL 原生文件系统中编译，避免 NTFS 9P 性能瓶颈
#
# 使用方法 (在 Windows PowerShell 中):
#   wsl -d Ubuntu-20.04 -- bash /mnt/e/整理开源文件/小黑侠遥控器/lv_xiaoheixia/fast-build.sh
#
# 参数:
#   -c  全量重新编译 (clean build)
#   -n  只编译不部署

set -e

SRC_WIN="/mnt/e/整理开源文件/小黑侠遥控器/lv_xiaoheixia"
SRC_WSL="$HOME/lv_xiaoheixia"
BUILD_DIR="$SRC_WSL/output"
TOOLCHAIN="$SRC_WSL/cmake/toolchainfile-wsl-arm.cmake"
JOBS=$(nproc)
CLEAN=false
NO_DEPLOY=false

# SuperDisplay adb v40 兼容系统后台 adb 服务，不会冲突
ADB="/mnt/c/Program Files/SuperDisplay/adb/adb.exe"
if [ ! -f "$ADB" ]; then
    ADB="adb.exe"
fi
DEVICE="145783923"

while getopts "cn" arg; do
    case $arg in
        c) CLEAN=true ;;
        n) NO_DEPLOY=true ;;
    esac
done

echo "=== 同步源码到 WSL 原生文件系统 ==="
rsync -a --delete "$SRC_WIN/" "$SRC_WSL/" --exclude output
echo "同步完成"

if [ "$CLEAN" = true ] || [ ! -f "$BUILD_DIR/Makefile" ]; then
    echo "=== CMake 配置 ==="
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake -DARCH=arm \
          -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
          -DCMAKE_INSTALL_PREFIX=./release \
          ..
fi

echo "=== 编译 (-j$JOBS) ==="
cd "$BUILD_DIR"
time make -j"$JOBS"

echo "=== 编译完成: $BUILD_DIR/lvgl_app ==="
ls -lh "$BUILD_DIR/lvgl_app"

if [ "$NO_DEPLOY" = true ]; then
    echo "跳过部署 (-n)"
    exit 0
fi

echo "=== 部署到设备 ==="
cp "$BUILD_DIR/lvgl_app" "$SRC_WIN/output/lvgl_app"

"$ADB" -s "$DEVICE" shell "start-stop-daemon -K -x /usr/bin/lvgl_app 2>/dev/null; true"
"$ADB" -s "$DEVICE" push "$SRC_WIN/output/lvgl_app" /tmp/lvgl_app
"$ADB" -s "$DEVICE" shell "cp /tmp/lvgl_app /usr/bin/lvgl_app; chmod +x /usr/bin/lvgl_app; start-stop-daemon -S -b -x /usr/bin/lvgl_app"

sleep 1
"$ADB" -s "$DEVICE" shell "ps | grep '[l]vgl_app'"
echo "=== 部署完成 ==="
