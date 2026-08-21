#!/bin/bash
set -e
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "==> BUILD_TYPE=$BUILD_TYPE"

mkdir -p "$BUILD_DIR"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
make -j$(nproc) -C "$BUILD_DIR"

echo "==> Build done."
echo "    主程序(Qt):   $BUILD_DIR/qtui/yolov8_sam_demo_qt"
echo "    headless:    $BUILD_DIR/yolov8_sam_demo"
echo ""
echo "==> 相机配置: docs/camera_setup.md (海康 MVS SDK)"
