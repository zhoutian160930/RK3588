#!/bin/bash
set -e
# ============================================================
# setup_camera.sh — OPT 相机一键配置（IP 发现 + SDK 安装）
# ============================================================

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---------- 配置项（改成你的实际值） ----------
CAMERA_IFACE="${CAMERA_IFACE:-eth1}"
BOARD_IP="${BOARD_IP:-169.254.100.100/16}"
# 如果已知相机 IP，填写后跳过 ARP 扫描
CAMERA_IP="${CAMERA_IP:-}"
SCICAM_SDK_DIR="${SCICAM_SDK_DIR:-/home/forlinx/SciCamSDK_V1.6.1.5_20250925/Development/Sample/BaseDemoConsole}"
# ---------- 配置项结束 ----------

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo -e "${RED}请用 root 运行: sudo $0${NC}"
        exit 1
    fi
}

step1_check_interface() {
    echo -e "\n${GREEN}=== Step 1: 检查网口 ${CAMERA_IFACE} ===${NC}"
    if ip link show "$CAMERA_IFACE" &>/dev/null; then
        echo "    $CAMERA_IFACE 存在, 状态: $(ip link show $CAMERA_IFACE | grep -oP 'state \K\w+')"
        ip link set "$CAMERA_IFACE" up 2>/dev/null || true
    else
        echo -e "${RED}    错误: $CAMERA_IFACE 不存在, 可用网口:${NC}"
        ip link show | grep -E "^[0-9]+:" | awk '{print "      " $2}' | tr -d ':'
        exit 1
    fi
}

step2_configure_ip() {
    echo -e "\n${GREEN}=== Step 2: 配置开发板 IP ===${NC}"
    local ip_addr=$(echo "$BOARD_IP" | cut -d'/' -f1)
    if ip addr show "$CAMERA_IFACE" | grep -q "$ip_addr"; then
        echo "    $CAMERA_IFACE 已配置 IP: $BOARD_IP (跳过)"
    else
        ip addr add "$BOARD_IP" dev "$CAMERA_IFACE"
        echo "    已配置: ip addr add $BOARD_IP dev $CAMERA_IFACE"
    fi
}

step3_find_camera() {
    echo -e "\n${GREEN}=== Step 3: 发现相机 ===${NC}"

    if [ -n "$CAMERA_IP" ]; then
        echo "    使用提供的相机 IP: $CAMERA_IP (跳过扫描)"
        return
    fi

    # 方式一: arp-scan
    if command -v arp-scan &>/dev/null; then
        echo "    使用 arp-scan 扫描 $CAMERA_IFACE ..."
        local result
        result=$(arp-scan --interface="$CAMERA_IFACE" --localnet 2>/dev/null | \
                 grep -i "Vector\|OPT\|00:02:c4" | head -1)
        if [ -n "$result" ]; then
            CAMERA_IP=$(echo "$result" | awk '{print $1}')
            echo -e "    ${GREEN}发现相机: $CAMERA_IP${NC} ($result)"
            return
        fi
        echo -e "    ${YELLOW}arp-scan 未发现相机${NC}"
    else
        echo -e "    ${YELLOW}arp-scan 未安装, 跳过 (apt install arp-scan)${NC}"
    fi

    # 方式二: 用 SDK fps_bench 发现
    local fps_bench="$SCICAM_SDK_DIR/build_Linux_ARM64/fps_bench"
    local lib_dir="$SCICAM_SDK_DIR/Libraries/linux_arm64"
    if [ -f "$fps_bench" ]; then
        echo "    使用 SDK fps_bench 发现相机..."
        local output
        output=$(LD_LIBRARY_PATH="$lib_dir" timeout 10 "$fps_bench" 2>&1 || true)
        if echo "$output" | grep -q "Found:"; then
            local model=$(echo "$output" | grep "Found:" | head -1)
            echo -e "    ${GREEN}$model${NC}"
            echo "    (执行成功，相机已可连接，跳过 IP 输出)"
            return
        fi
        echo -e "    ${YELLOW}SDK 也未发现相机，请检查相机电源和网线${NC}"
    fi
}

step4_install_sdk() {
    echo -e "\n${GREEN}=== Step 4: 安装 SciCamSDK 到系统 ===${NC}"
    if [ ! -f "/usr/lib/libSciCamSDK.so" ] || [ ! -f "/usr/lib/GEVTLSCI.cti" ]; then
        echo "    执行 install_sdk.sh ..."
        BASEDIR="$(cd "$(dirname "$0")" && pwd)"
        SCICAM_SDK_DIR="$SCICAM_SDK_DIR" "$BASEDIR/install_sdk.sh"
    else
        echo "    SciCamSDK 已安装 (跳过)"
    fi
}

step5_show_config() {
    echo -e "\n${GREEN}=== Step 5: 配置参数 ===${NC}"
    echo "    在 config/parameters.json 中确认以下值:"
    echo ""
    echo '    "camera_enabled": true,'
    echo '    "camera_iface": "'"$CAMERA_IFACE"'",'
    echo '    "camera_ip": "'"$BOARD_IP"'",'
    echo ""
    echo -e "    ${YELLOW}camera_ip 是开发板的 IP, 不是相机的 IP${NC}"
    if [ -n "$CAMERA_IP" ]; then
        echo -e "    相机 IP: ${GREEN}$CAMERA_IP${NC}"
    fi
}

step6_test() {
    echo -e "\n${GREEN}=== Step 6: 验证相机 ===${NC}"
    local fps_bench="$SCICAM_SDK_DIR/build_Linux_ARM64/fps_bench"
    local lib_dir="$SCICAM_SDK_DIR/Libraries/linux_arm64"
    if [ -f "$fps_bench" ]; then
        echo "    运行 fps_bench 验证..."
        LD_LIBRARY_PATH="$lib_dir" timeout 8 "$fps_bench" 2>&1 | grep -E "Found:|FPS|Exposure" || true
        echo -e "    ${GREEN}验证完成${NC}"
    else
        echo "    fps_bench 不存在, 跳过验证"
    fi
}

# ===================== 主流程 =====================
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  OPT 相机一键配置${NC}"
echo -e "${GREEN}============================================${NC}"

require_root
step1_check_interface
step2_configure_ip
step3_find_camera
step4_install_sdk
step5_show_config
step6_test

echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  配置完成!${NC}"
echo -e "${GREEN}  运行: sudo ./build/yolov8_sam_demo${NC}"
echo -e "${GREEN}============================================${NC}"
