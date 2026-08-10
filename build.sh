#!/bin/bash
set -e
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

# ---------- paths (override as needed) ----------
LVGL_SRC="${LVGL_SRC:-/home/forlinx/lvgl-release-v8.3}"
LV_DRIVERS_PARENT="${LV_DRIVERS_PARENT:-/home/forlinx/lv_port_linux-release-v8.3}"
LVGL_BACKEND="${LVGL_BACKEND:-SDL}"          # SDL or FBDEV
BUILD_TYPE="${BUILD_TYPE:-Release}"

echo "==> LVGL_SRC=$LVGL_SRC"
echo "==> LV_DRIVERS_PARENT=$LV_DRIVERS_PARENT"
echo "==> LVGL_BACKEND=$LVGL_BACKEND"
echo "==> BUILD_TYPE=$BUILD_TYPE"

mkdir -p "$BUILD_DIR"
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLVGL_SRC_DIR="$LVGL_SRC" \
    -DLV_DRIVERS_PARENT="$LV_DRIVERS_PARENT" \
    -DLVGL_BACKEND="$LVGL_BACKEND"

make -j$(nproc) -C "$BUILD_DIR"

echo "==> Build done."
echo "    主程序: $BUILD_DIR/yolov8_sam_demo"
echo "    UI 测试: $BUILD_DIR/ui/ui_test"
