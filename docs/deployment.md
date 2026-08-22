# 新板部署指南(推块物料智能视觉检测系统)

适用平台:飞凌 OK3588-C(RK3588 / aarch64 / Ubuntu 22.04,带桌面环境)+ 海康 GigE 工业相机 + Qt 检测程序。

> 全文假设:用户名 `forlinx`、相机接 **eth1**、SSH/上网走 eth0。
> 用户名或路径不同时,先看 **附录 A** 做路径替换,再按章节操作。

---

## 0. 部署前检查清单

| # | 检查项 | 命令 | 期望 |
|---|--------|------|------|
| 1 | NPU 驱动 | `ls /dev/rknpu*` | 存在(飞凌固件自带) |
| 2 | RGA 驱动 | `ls /dev/rga /dev/dma_heap/` | 存在 |
| 3 | GPIO 扩展芯片(TCA6424) | `ls /sys/class/gpio/gpiochip485` | 存在(飞凌板载) |
| 4 | 桌面环境 | 有 GNOME 桌面可登录 | Qt 界面需要 |
| 5 | 磁盘空间 | `df -h /` | ≥ 2G 可用 |

相机型号无硬性要求(实测 MV-CU013-80GC),其他海康 GigE 型号同理。

---

## 1. 系统依赖(联网 apt)

```bash
sudo apt update
sudo apt install -y build-essential cmake \
    qtbase5-dev libopencv-dev libspdlog-dev libfmt-dev \
    fonts-noto-cjk
```

验证:

```bash
qmake --version          # Qt 5.15.x
pkg-config --modversion opencv4   # 4.5+
gcc --version            # 9+ 即可(实测 11.4)
```

> NPU 运行库 `librknnrt.so`、RGA 库 `librga.so` 已随仓库 `lib/` 目录自带,**无需安装**。

---

## 2. 海康 MVS SDK

### 2.1 安装(二选一)

**方式 A:官方脚本装到 /opt(root 分区空间充足时,最简单)**

```bash
tar xzf MVS-5.0.2_aarch64_*.tar.gz
cd MVS-5.0.2_aarch64_*/
sudo ./setup.sh        # RK3588 平台自动装到 /opt/MVS,无交互
```

**方式 B:装到 /userdata(root 分区紧张时,推荐,当前板即此方案)**

```bash
# 解出内层 MVS.tar.gz 再展开到 /userdata,并做软链接保持兼容
tar xzf MVS-5.0.2_aarch64_*.tar.gz
cd MVS-5.0.2_aarch64_*/
tar xzf MVS.tar.gz                     # 解出 MVS/ 目录(bin/lib/include/...)
sudo mv MVS /userdata/MVS
sudo ln -sfn /userdata/MVS /opt/MVS
```

### 2.2 GigE 必配项(两种方式装完都执行)

```bash
# SDK 日志服务(开机自启)
cd /opt/MVS/logserver && sudo ./InstallServer.sh

# 关闭反向路径过滤(否则跨网段发现/取流可能失败)
sudo bash /opt/MVS/bin/set_rp_filter.sh --mode=0 --restartnetworkstack=no

# 放大 socket 缓冲(高帧率取流需要)
sudo bash /opt/MVS/bin/set_socket_buffer_size.sh
```

### 2.3 验证

```bash
python3 scripts/mvs_list_devices.py
# 期望输出: 发现 1 台相机 ... IP:192.168.1.64(新相机可能是 0.0.0.0 或其他)
```

> 工程 CMake 会自动探测 SDK 位置(/userdata/MVS 或 /opt/MVS);
> 装在其他路径时:`cmake -B build -DMVS_SDK_DIR=/你的路径 ..`

---

## 3. 网络与相机 IP(一次性)

板端 eth1 固定 `192.168.1.100/24`,相机固定 `192.168.1.64`(写入相机持久存储,断电保留,换主机不用重配)。

### 3.1 板端 eth1

```bash
# 连接名以 nmcli con show 实际输出为准(示例为 "Wired connection 2")
nmcli con show
sudo nmcli con mod "Wired connection 2" ipv4.method manual \
     ipv4.addresses 192.168.1.100/24 ipv4.gateway "" ipv4.dns ""
sudo nmcli con up "Wired connection 2"
ip addr show eth1        # 应看到 inet 192.168.1.100/24
```

### 3.2 相机 IP

```bash
# ForceIP 救活 + 写入持久静态 IP(相机当前 0.0.0.0/乱 IP 时同样适用)
python3 scripts/mvs_configure_ip.py                 # 默认 192.168.1.64
# 多相机时逐台接、逐台配唯一 IP:
# python3 scripts/mvs_configure_ip.py --ip 192.168.1.65
```

### 3.3 验证

```bash
ping -c3 192.168.1.64      # 0% loss
```

> ⚠ 多台海康相机出厂默认 IP 相同(192.168.1.64),同时接入会冲突;
> 必须一台一台单独配置成唯一 IP 后再并网。

---

## 4. 获取代码并编译

```bash
# git 克隆(或从旧板 rsync 整个目录)
git clone git@github.com:zhoutian160930/RK3588.git ~/lvgl/RK3588

cd ~/lvgl/RK3588
./build.sh
```

产物:

| 文件 | 用途 |
|------|------|
| `build/yolov8_sam_demo_qt` | 主程序(Qt 全屏界面) |
| `build/yolov8_sam_demo` | headless CLI(调试用) |

> 换板后若 build/ 是旧板产物,先 `rm -rf build && ./build.sh`(缓存里有旧板绝对路径)。

---

## 5. 运行配置(parameters.json)

编辑 `config/parameters.json`,关键字段:

| 字段 | 说明 | 参考值 |
|------|------|--------|
| `camera_enabled` | 启动即加载相机 | `true` |
| `camera_iface` | 相机所在网口 | `"eth1"` |
| `camera_ip` | **板端**网口 IP(CIDR) | `"192.168.1.100/24"` |
| `camera_width/height` | 采集 ROI,0=满分辨率 | `0` / `0` |
| `camera_exposure_us` | 曝光:0=自动(默认),>0=手动微秒值,热加载即时生效 | `0` |
| `camera_gain` | 增益 dB:-1=自动(默认),>=0=手动,热加载即时生效 | `-1.0` |
| `yolo_model` | RKNN 模型绝对路径 | `/home/forlinx/lvgl/yolomodel/曲奇.rknn` |
| `label_path` | 类别标签 | `/home/forlinx/lvgl/yolomodel/classes.txt` |
| `box_class` / `material_class` | 盒/物料的类别 ID | `1` / `0` |
| `target_count` | 满料目标数 | 按产线 |
| `can_enabled` 等 | CAN 结果上报 | 见 8 节 |
| `gpio_enabled` 等 | GPIO 合格信号(sysfs,需 root) | `true` |

**模型文件**需单独拷贝(不在仓库内):`~/lvgl/yolomodel/*.rknn + classes.txt`。

---

## 6. root 运行配置(GPIO 必需,一次性)

GPIO 走 sysfs(`/sys/class/gpio`),必须 root 才能写。配置免密白名单后,桌面图标点开即以 root 运行:

```bash
cd ~/lvgl/RK3588
sudo cp scripts/90-yolov8-root.sudoers /etc/sudoers.d/90-yolov8-root
sudo chmod 440 /etc/sudoers.d/90-yolov8-root
```

验证(不应提示密码):

```bash
sudo -n -l 2>/dev/null | tail -2
# 应包含: (root) NOPASSWD: /home/forlinx/lvgl/RK3588/build/yolov8_sam_demo_qt
```

> 白名单只授权**该二进制**和一条固定参数的 chown(运行后把输出文件属主归还普通用户),不影响其他 sudo 命令。
> ⚠ sudoers.d 内文件名不能带 `.`,sudo 会忽略,所以安装时必须重命名为 `90-yolov8-root`。

---

## 7. 桌面图标(一次性)

```bash
# 图标
mkdir -p ~/.local/share/icons
cp scripts/yolov8-detection.png ~/.local/share/icons/

# 桌面入口
cp scripts/yolov8-detection.desktop ~/.local/share/applications/
update-desktop-database ~/.local/share/applications/ 2>/dev/null

# (可选)固定到左侧任务栏:
gsettings get org.gnome.shell favorite-apps
# 把输出的列表里加上 'yolov8-detection.desktop'(放最前),再 set 回去,示例:
gsettings set org.gnome.shell favorite-apps "['yolov8-detection.desktop', 'chromium-browser.desktop']"
```

---

## 8. CAN(可选)

程序通过 SocketCAN 上报判定结果(合格发 0x00,不合格发 0x01,ID 默认 0x300)。
不接 CAN 时把 `parameters.json` 的 `can_enabled` 设 `false`(否则日志持续报 Network is down,无害但刷屏)。

接 CAN 时:

```bash
sudo ip link set can0 down 2>/dev/null
sudo ip link set can0 up type can bitrate 500000    # 波特率按产线 PLC
```

---

## 9. 首次启动验证

桌面点击"推块物料智能视觉检测系统"(或 `./run_as_root.sh`),核对日志(界面"警告和报警"页或 `output/log/*.txt`):

| 日志 | 期望 |
|------|------|
| `camera: 发现 ... IP:192.168.1.64` → `camera: 就绪 1280x1024` | 相机链路 OK |
| `GPIO: P23 ... 输出就绪` / `GPIO-in: P27 ... 输入就绪` | root + GPIO OK(若显示"失败(需 root)"说明没走 run_as_root.sh) |
| `RGA dma-buf letterbox 预处理生效` | NPU 预处理硬件加速 OK |
| 点"开始"后 `frame N 合格=...` | 推理链路 OK |

**注意**:

- 程序**同一时刻只能开一个实例**(海康相机独占);发现"相机没了"先查后台残留进程 `ps aux | grep yolov8`。
- `CAN-send ... Network is down`:can0 没起,见第 8 节。
- 预处理 A/B 对比:`RKNN_FORCE_CPU_PREPROCESS=1 ./run_as_root.sh` 强制 CPU 路径。

---

## 10. 常见问题(FAQ)

| 现象 | 原因/处理 |
|------|-----------|
| 编译报 `未找到海康 MVS SDK` | SDK 未装或路径特殊,`cmake -B build -DMVS_SDK_DIR=/路径 ..` |
| `未发现相机` | ① 相机被其他实例独占(单实例) ② 网线/供电 ③ eth1 IP 没配(3.1) ④ rp_filter 没关(2.2) |
| `OpenDevice 失败 (0x80000203)` | 设备无访问权限 = 被其他进程占用,杀掉残留实例 |
| 相机 IP 变成 0.0.0.0 | 断电丢临时配置,重跑 `scripts/mvs_configure_ip.py`(持久配置一般不受影响) |
| GPIO `Permission denied` | 程序没用 root 跑:从桌面图标(经 run_as_root.sh)启动,或 `sudo ./build/yolov8_sam_demo_qt` |
| 检测图/日志是 root 属主删不掉 | 正常路径会自动 chown 归还;手动修:`sudo chown -R forlinx:forlinx ~/lvgl/output` |
| 界面中文乱码/方块 | `sudo apt install fonts-noto-cjk` 后重启程序 |
| 换了非 16 对齐分辨率的相机 | 自动走 CPU 预处理(功能不变),日志无 RGA 生效条目属预期 |

---

## 附录 A:用户名/路径不同时

仓库内以下文件含硬编码 `/home/forlinx`,换用户名先替换:

```bash
cd ~/lvgl/RK3588
sed -i "s|/home/forlinx|$HOME|g" \
    run_as_root.sh scripts/90-yolov8-root.sudoers scripts/yolov8-detection.desktop
# 替换后重新执行第 6 节(sudoers 安装)和第 7 节(桌面图标),
# 并同步修改 config/parameters.json 内的绝对路径。
```

## 附录 B:本仓库部署相关文件一览

| 文件 | 用途 |
|------|------|
| `run_as_root.sh` | 桌面 root 启动入口(免密 sudo → pkexec 兜底 + 输出属主归还) |
| `scripts/90-yolov8-root.sudoers` | sudo 免密白名单源文件 |
| `scripts/yolov8-detection.desktop` | 桌面入口模板 |
| `scripts/yolov8-detection.png` | 应用图标 |
| `scripts/mvs_list_devices.py` | 枚举相机(部署验证) |
| `scripts/mvs_configure_ip.py` | 相机 ForceIP + 持久静态 IP |
| `lib/librknnrt.so` / `librga.so` | NPU / RGA 运行库(随仓库走) |
| `docs/camera_setup.md` | 相机与 SDK 细节文档 |
