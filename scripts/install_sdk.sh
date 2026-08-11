#!/bin/bash
set -e
# ============================================================
# install_sdk.sh — OPT SciCamSDK ARM64 系统级安装
# ============================================================

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 root 运行: sudo $0"
    exit 1
fi

# ---------- 在这里改 SDK 路径 ----------
SCICAM_SDK_DIR="${SCICAM_SDK_DIR:-/home/forlinx/SciCamSDK_V1.6.1.5_20250925/Development/Sample/BaseDemoConsole}"
LIB_DIR="${SCICAM_SDK_DIR}/Libraries/linux_arm64"

if [ ! -d "$LIB_DIR" ]; then
    echo "错误: SDK 库目录不存在: $LIB_DIR"
    echo "请设置 SCICAM_SDK_DIR 指向 SciCamSDK 安装路径"
    exit 1
fi

echo "==> 安装 SciCamSDK 到系统..."
echo "    SDK 路径: $SCICAM_SDK_DIR"

# 库文件
echo "    复制 .so 文件..."
cp -v "$LIB_DIR"/*.so* /usr/lib/ 2>/dev/null || true

# GenICam 传输层配置文件（关键！缺失会导致找不到相机）
echo "    复制 GenICam 传输层配置..."
cp -v "$LIB_DIR"/GEVTLSCI.cti /usr/lib/ 2>/dev/null || true
cp -rv "$LIB_DIR"/GevCtiRes /usr/lib/ 2>/dev/null || true

# 更新动态链接器缓存
ldconfig

echo ""
echo "==> 验证"
echo "    ldconfig -p | grep SciCamSDK:"
ldconfig -p | grep SciCamSDK || echo "    (库未在缓存中，检查 /usr/lib/)"
echo "    ls /usr/lib/GEVTLSCI.cti:"
ls -la /usr/lib/GEVTLSCI.cti 2>/dev/null || echo "    缺失!"
echo "    ls /usr/lib/GevCtiRes:"
ls /usr/lib/GevCtiRes/ 2>/dev/null | head -3 || echo "    缺失!"

echo ""
echo "==> SDK 安装完成"
echo "    编译项目后，sudo ./yolov8_sam_demo 即可启动相机"
