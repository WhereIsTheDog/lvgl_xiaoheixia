#!/bin/bash
# 快速编译脚本 - 当前目录编译
set -e

SRC_DIR="/mnt/c/Users/nickfu/Documents/cli/xiaoheixia/lv_xiaoheixia_extracted/lv_xiaoheixia"
SRC_WSL="$HOME/lv_xiaoheixia_temp"
BUILD_DIR="$SRC_WSL/output"
TOOLCHAIN="$SRC_WSL/cmake/toolchainfile-rv1106.cmake"
JOBS=$(nproc)

echo "=== 同步源码到 WSL 原生文件系统 ==="
rsync -a --delete "$SRC_DIR/" "$SRC_WSL/" --exclude output --exclude build
echo "同步完成"

echo "=== CMake 配置 ==="
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# 检查toolchain文件是否存在
if [ ! -f "$TOOLCHAIN" ]; then
    echo "错误: 找不到 toolchain 文件: $TOOLCHAIN"
    echo "尝试查找可用的 toolchain 文件..."
    find "$SRC_WSL/cmake" -name "*.cmake" -type f
    exit 1
fi

cmake -DARCH=arm \
      -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
      -DCMAKE_INSTALL_PREFIX=./release \
      ..

echo "=== 编译 (-j$JOBS) ==="
time make -j"$JOBS"

echo "=== 编译完成 ==="
ls -lh "$BUILD_DIR/lvgl_app"

echo "=== 复制可执行文件回 Windows ==="
mkdir -p "$SRC_DIR/output"
cp "$BUILD_DIR/lvgl_app" "$SRC_DIR/output/lvgl_app"

echo "=== 编译成功! 输出文件: $SRC_DIR/output/lvgl_app ==="
