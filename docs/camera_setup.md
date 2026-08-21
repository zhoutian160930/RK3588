# 海康工业相机配置指南

## 概述

本文档说明如何在 RK3588 开发板上配置海康威视（Hikrobot）工业相机，配合 YOLOv8-SAM 检测系统使用。

**硬件环境**: Rockchip RK3588 + 海康 GigE 工业相机（MV-CU013-80GC 等）
**软件方案**: 海康 MVS SDK ARM64 原生驱动（`libMvCameraControl`）

---

## 1. IP 配置（已完成，掉电保留）

| 端 | IP | 说明 |
|----|-----|------|
| 开发板 eth1 | `192.168.1.100/24` | NetworkManager 持久配置 |
| 相机 | `192.168.1.64/24` | 已写入相机持久存储（STATIC） |

如需重新配置（例如换相机或改网段）：

```bash
# 板端(eth1 连接名以 nmcli con show 为准)
sudo nmcli con mod "Wired connection 2" ipv4.method manual \
     ipv4.addresses 192.168.1.100/24 ipv4.gateway "" ipv4.dns ""
sudo nmcli con up "Wired connection 2"

# 相机(ForceIP 救援 + 持久化, 相机 IP 未知/0.0.0.0 时也适用)
python3 /userdata/workspace/mvs_demo/configure_hik_ip.py
```

## 2. MVS SDK 安装（已完成）

- SDK 安装于 `/userdata/MVS`（`/opt/MVS` 为其软链接）
- 头文件: `/userdata/MVS/include`，库: `/userdata/MVS/lib/aarch64`
- LogServer 服务、rp_filter=0、usbfs=2000M、socket 缓冲均已配置
- 官方例程: `/userdata/MVS/Samples/aarch64/`

## 3. parameters.json 相机字段

```json
{
  "camera_enabled": true,
  "camera_iface": "eth1",
  "camera_ip": "192.168.1.100/24",
  "camera_timeout_ms": 10000,
  "camera_width": 0,
  "camera_height": 0
}
```

| 字段 | 说明 | 默认值 |
|------|------|--------|
| `camera_enabled` | 是否启用相机 | `true` |
| `camera_iface` | 相机连接的网口 | `"eth1"` |
| `camera_ip` | **开发板**在该网口的 IP（CIDR） | `"192.168.1.100/24"` |
| `camera_timeout_ms` | 等待相机就绪超时 | `10000` |
| `camera_width/height` | ROI，0=传感器满分辨率 | `0` |

> **注意**: `camera_ip` 是给开发板网口配的 IP，**不是**相机的 IP。只要和相机同网段即可。

---

## 4. 相机采集架构

```
┌──────────────────────────────────────────────────────────┐
│                   yolov8_sam_demo                        │
│                                                          │
│  ui_app.cpp                  camera_capture.cpp          │
│  ┌──────────┐   grab()      ┌──────────────────────┐    │
│  │ UI 预览   │ ───────────→ │ MV_CC_GetImageBuffer │    │
│  │ worker_fn │              │ OpenCV 转 BGR        │    │
│  └──────────┘              │ (Bayer/Mono/RGB)     │    │
│                              └────────┬─────────────┘    │
│                                       │                  │
└───────────────────────────────────────┼──────────────────┘
                                        │ C API
                              ┌─────────▼─────────────┐
                              │ libMvCameraControl.so │
                              │ (海康 MVS ARM64 SDK)  │
                              └─────────┬─────────────┘
                                        │ GigE Vision
                              ┌─────────▼─────────────┐
                              │  海康工业相机          │
                              │  MV-CU013-80GC        │
                              │  (1280×1024 BayerRG8) │
                              └───────────────────────┘
```

**采集流程**:

1. `init()`: 配置 eth1 IP → `MV_CC_EnumDevices` → `CreateHandleWithoutLog` → `OpenDevice` → 最优包长 → 自动曝光/增益 → ROI → `LatestImages` 策略(缓存 2 节点) → `StartGrabbing`
2. `grab()`: `MV_CC_GetImageBuffer` → OpenCV `cvtColor`(BayerRG8→BGR / Mono8→BGR / RGB8→BGR) → `MV_CC_FreeImageBuffer`
3. `shutdown()`: `StopGrabbing` → `CloseDevice` → `DestroyHandle`

---

## 5. API 参数速查

### 5.1 相机控制节点

通过 `MV_CC_SetXXXValue(handle, key, val)` 读写：

| Key | 类型 | 说明 | 示例值 |
|-----|------|------|--------|
| `ExposureAuto` | Enum | 自动曝光 | `0`=Off, `1`=Once, `2`=Continuous |
| `ExposureTime` | Float | 曝光时间(µs) | `31` ~ `988930` |
| `GainAuto` | Enum | 自动增益 | `0`=Off, `1`=Once, `2`=Continuous |
| `Gain` | Float | 增益倍数 | 见相机规格 |
| `Width` | Int | 图像宽度 | 需按 nInc 对齐 |
| `Height` | Int | 图像高度 | 需按 nInc 对齐 |
| `OffsetX/Y` | Int | ROI 偏移 | 居中: `(Wmax-W)/2` 对齐 |
| `GevSCPSPacketSize` | Int | 包大小(MTU) | 用 `MV_CC_GetOptimalPacketSize()` |

### 5.2 采集策略

```c
enum MV_GRAB_STRATEGY {
    MV_GrabStrategy_OneByOne         = 0,  // 按序获取（默认）
    MV_GrabStrategy_LatestImagesOnly = 1,  // 只取最新一帧
    MV_GrabStrategy_LatestImages     = 2,  // 取最新图像
    MV_GrabStrategy_UpcomingImage    = 3,  // 等下一帧
};
```

当前使用 `LatestImages` + `MV_CC_SetImageNodeNum(2)`（低延迟，防 UI 滞后）。

---

## 6. 常见问题

### Q: 程序提示"未发现相机"

1. 检查相机电源/网线（eth1 link 灯）
2. `ping 192.168.1.64` 确认可达
3. `ip addr show eth1` 确认板端 IP 为 192.168.1.100/24
4. 用官方例程验证: `cd /userdata/MVS/Samples/aarch64/C++/General/GrabImage && make && ./GrabImage`

### Q: 画面很暗

自动曝光默认开启。若需手动曝光：

```c
MV_CC_SetEnumValue(handle, "ExposureAuto", 0);   // Off
MV_CC_SetFloatValue(handle, "ExposureTime", 50000); // 50ms
```

### Q: 需要 root 权限

海康 GigE 取流走 UDP，普通用户即可运行（无需 sudo）。

### Q: 如何切换 ROI 提高帧率

修改 `parameters.json` 的 `camera_width/camera_height`（如 `1280/1024`），重启程序生效；`0` 为传感器满分辨率居中复位。

---

## 7. 文件清单

```
RK3588/
├── core/
│   └── camera_capture.cpp   # 相机采集模块（集成海康 MVS SDK）
├── config/
│   └── parameters.json      # 运行时配置
└── docs/
    └── camera_setup.md      # 本文档
```

---

## 8. 依赖总结

| 组件 | 用途 | 位置 |
|------|------|------|
| libMvCameraControl.so | 海康相机驱动 | `/userdata/MVS/lib/aarch64` |
| MVS 头文件 | 编译依赖 | `/userdata/MVS/include` |
| MvLogServer | SDK 日志服务 | `/userdata/MVS/logserver`（已装系统服务） |
