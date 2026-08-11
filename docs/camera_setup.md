# OPT 工业相机配置指南

## 概述

本文档详细说明如何在 RK3588 开发板上配置 OPT（奥普特）工业相机，配合 YOLOv8-SAM 检测系统使用。

**硬件环境**: Rockchip RK3588 + OPT GigE 工业相机  
**软件方案**: 奥普特 SciCamSDK ARM64 原生驱动（无 qemu 模拟）

---

## 1. 一键配置

```bash
sudo ./scripts/setup_camera.sh
```

该脚本自动完成：网口检查 → IP 配置 → 相机发现 → SDK 安装 → 参数确认。

## 2. 物理连接

```
  OPT 相机                            RK3588 开发板
┌──────────┐                        ┌──────────────┐
│  网口    │ ──── 网线(直连) ────→  │   eth1       │
│  电源    │ ──── 24V DC        │   │              │
└──────────┘                        └──────────────┘
```

- 相机网口直接连接开发板对应网口（默认 `eth1`）
- 无需经过交换机，直连即可
- 相机需单独供电（24V DC）

---

## 3. 手动配置步骤

### 3.1 检查网口状态

```bash
ip link show eth1
```

如果状态是 `DOWN`，先启用：

```bash
ip link set eth1 up
```

### 3.2 配置开发板 IP

OPT 相机出厂使用 `169.254.x.x`（链路本地地址），开发板需配置同网段 IP：

```bash
ip addr add 169.254.100.100/16 dev eth1
```

> `169.254.100.100` 可换成任意 `169.254.x.x`，只要不和相机 IP 冲突即可。`/16` 表示子网掩码 `255.255.0.0`。

### 3.3 查找相机 IP

**方式一：ARP 扫描（最快）**

```bash
apt install arp-scan -y
ip addr add 169.254.100.100/16 dev eth1
arp-scan --interface=eth1 --localnet
```

输出中查找 `Vector International` 或 MAC 前缀 `00:02:c4`，对应 IP 即为相机地址。

示例输出：
```
169.254.165.2   00:02:c4:39:fc:5b   Vector International BVBA
                                              ↑
                                          相机 IP
```

**方式二：用 SDK 工具发现**

```bash
cd /path/to/SciCamSDK/.../build_Linux_ARM64
LD_LIBRARY_PATH=../Libraries/linux_arm64 ./fps_bench
```

成功时会打印 `Found: OPT-CC1-...`。

**方式三：抓 ARP 包（无需知道 IP）**

```bash
tcpdump -i eth1 arp -n
```

相机上电时会主动发送 ARP 广播，输出中可见其 IP。

### 3.4 安装 SciCamSDK

SDK 库文件需安装到系统路径，包括几个关键文件：

```bash
SDK_DIR=/path/to/SciCamSDK_V1.6.1.5/Development/Sample/BaseDemoConsole/Libraries/linux_arm64

# 1. 动态库
sudo cp $SDK_DIR/*.so* /usr/lib/

# 2. GenICam 传输层配置文件（关键！）
sudo cp $SDK_DIR/GEVTLSCI.cti /usr/lib/
sudo cp -r $SDK_DIR/GevCtiRes /usr/lib/

# 3. 更新链接器缓存
sudo ldconfig
```

> **重要**: `GEVTLSCI.cti` 和 `GevCtiRes/` 必须和 `.so` 文件在同一目录。缺失这两个文件会导致 `SciCam_DiscoveryDevices()` 返回 0 台设备。

验证安装：

```bash
ldconfig -p | grep SciCamSDK     # 应显示 libSciCamSDK.so
ls /usr/lib/GEVTLSCI.cti        # 应存在
ls /usr/lib/GevCtiRes/           # 应有多个 XML 文件
```

### 3.5 配置 parameters.json

编辑 `config/parameters.json`，确认以下字段：

```json
{
  "camera_enabled": true,
  "camera_iface": "eth1",
  "camera_ip": "169.254.100.100/16",
  "camera_timeout_ms": 10000
}
```

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `camera_enabled` | 是否启用相机 | `true` |
| `camera_iface` | 相机连接的网口 | `"eth1"` |
| `camera_ip` | **开发板**在该网口的 IP（CIDR） | `"169.254.100.100/16"` |
| `camera_timeout_ms` | 等待相机就绪超时 | `10000` |

> **注意**: `camera_ip` 是给开发板网口配的 IP，**不是**相机的 IP。只要和相机同网段即可。

---

## 4. 相机采集架构

```
┌──────────────────────────────────────────────────────────┐
│                   yolov8_sam_demo                        │
│                                                          │
│  ui_app.cpp                  camera_capture.cpp          │
│  ┌──────────┐   grab()      ┌──────────────────────┐    │
│  │ UI 预览   │ ───────────→ │ SciCam_Grab()        │    │
│  │ worker_fn │              │ Payload_GetImage()   │    │
│  └──────────┘              │ Payload_ConvertImage │    │
│                              │ → cv::Mat BGR        │    │
│                              └────────┬─────────────┘    │
│                                       │                  │
└───────────────────────────────────────┼──────────────────┘
                                        │ C API
                              ┌─────────▼─────────────┐
                              │   libSciCamSDK.so     │
                              │   (OPT ARM64 SDK)     │
                              └─────────┬─────────────┘
                                        │ GigE Vision
                              ┌─────────▼─────────────┐
                              │   OPT 工业相机         │
                              │   (GigE, 2592×1944)   │
                              └───────────────────────┘
```

**采集流程**:

1. `init()`: 配置 eth1 IP → `SciCam_DiscoveryDevices` → `CreateDevice` → `OpenDevice` → 自动曝光/增益 → `StartGrabbing`
2. `grab()`: `SciCam_Grab` → `Payload_GetImage` → `ConvertImage(Mono8)` → `cvtColor(GRAY2BGR)` → 返回 `cv::Mat`
3. `shutdown()`: `StopGrabbing` → `CloseDevice` → `DeleteDevice`

---

## 5. API 参数速查

### 5.1 相机控制节点

通过 `SciCam_SetXXXValue(handle, key, val)` 读写：

| Key | 类型 | 说明 | 示例值 |
|-----|------|------|--------|
| `ExposureAuto` | Enum | 自动曝光 | `0`=Off, `1`=Once, `2`=Continuous |
| `ExposureTime` | Float | 曝光时间(µs) | `10000` ~ `996132` |
| `GainAuto` | Enum | 自动增益 | `0`=Off, `1`=Once, `2`=Continuous |
| `Gain` | Float | 增益倍数 | `1.0` ~ `31.98` |
| `Width` | Int | 图像宽度 | `2592` |
| `Height` | Int | 图像高度 | `1944` |
| `OffsetX` | Int | ROI 水平偏移 | `0` |
| `OffsetY` | Int | ROI 垂直偏移 | `0` |
| `AcquisitionFrameRate` | Float | 帧率限制 | `99999`(最大) |
| `GevSCPSPacketSize` | Int | 包大小(MTU) | `1500` ~ `9000` |

### 5.2 采集策略

```c
enum SciCamGrabStrategy {
    SciCam_GrabStrategy_OneByOne = 0,   // 按序获取（默认）
    SciCam_GrabStrategy_Latest   = 1,   // 只取最新帧
    SciCam_GrabStrategy_Upcoming = 2,   // 等下一帧
};
```

当前使用 `OneByOne`（不丢帧）。

---

## 6. 常见问题

### Q: 程序提示"未发现相机"

1. 检查相机电源是否接通
2. 检查 `config/parameters.json` 中 `camera_iface` 是否正确
3. 检查 `/usr/lib/GEVTLSCI.cti` 和 `/usr/lib/GevCtiRes/` 是否存在
4. 用 `fps_bench` 验证: `cd SDK目录/build_Linux_ARM64 && LD_LIBRARY_PATH=../Libraries/linux_arm64 ./fps_bench`

### Q: 画面很暗

自动曝光已默认开启。如果仍然暗，检查：

```bash
cd SDK目录/build_Linux_ARM64
LD_LIBRARY_PATH=../Libraries/linux_arm64 ./fps_bench 2>&1 | grep Exposure
```

输出 `ExposureTime: 46000 us` 表示曝光正常。如果仍为 10000µs，说明自动曝光未生效，可手动设置：

```c
SciCam_SetEnumValue(handle, "ExposureAuto", 2);  // Continuous
SciCam_SetFloatValue(handle, "ExposureTime", 50000);
```

### Q: 需要 root 权限

GigE Vision 需要 raw socket 访问网卡，必须用 `sudo` 运行主程序。未来可考虑用 `setcap` 授权：

```bash
sudo setcap cap_net_raw+ep ./build/yolov8_sam_demo
```

### Q: 如何切换 ROI 提高帧率

```c
SciCam_SetIntValue(handle, "Width", 1280);
SciCam_SetIntValue(handle, "Height", 1024);
SciCam_SetIntValue(handle, "OffsetX", 0);
SciCam_SetIntValue(handle, "OffsetY", 0);
```

设置后自动生效，无需重新打开相机。

---

## 7. 文件清单

```
RK3588/
├── scripts/
│   ├── setup_camera.sh      # 一键配置脚本
│   └── install_sdk.sh       # SDK 系统安装脚本
├── core/
│   └── camera_capture.cpp   # 相机采集模块（集成 SciCamSDK）
├── config/
│   └── parameters.json      # 运行时配置
└── docs/
    └── camera_setup.md      # 本文档
```

---

## 8. 依赖总结

| 组件 | 用途 | 安装方式 |
|------|------|----------|
| SciCamSDK *.so | OPT 相机驱动 | 执行 `scripts/install_sdk.sh` |
| GEVTLSCI.cti | GenICam GigE 传输层 | 同上 |
| GevCtiRes/*.xml | 传输层配置文件 | 同上 |
| arp-scan | 相机 IP 发现（可选） | `apt install arp-scan` |
