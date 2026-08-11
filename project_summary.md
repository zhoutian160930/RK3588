# 项目摘要：`/home/forlinx/lvgl`

## 一、项目定位

**RK3588 YOLOv8-SAM 工业视觉检测系统**

部署于 Rockchip RK3588 平台的产线视觉检测设备，使用 YOLOv8（NPU 加速）+ 可选 MobileSAM 分割，对工业产品（如曲奇饼干、藏药等包装盒）实时检测装料完整性，判断满料合格性，并通过 **CAN 总线 + GPIO（TCA6424）** 与产线 PLC 联动，配 **LVGL + SDL2 触屏界面** 供现场操作。

- **目标硬件**：RK3588 开发板（aarch64 Linux）+ OPT（奥普特）GigE 工业相机
- **典型场景**：食品/药品包装产线上检测包装盒内物料是否装够数量（如 4 个曲奇）
- **输出结果**：满料合格 / 未满 / 未漏出，实时发送给执行机构

---

## 二、顶层目录结构

```
/home/forlinx/lvgl/
├── RK3588/        # 【核心】C++ 源码工程（git 仓库），全部源代码与构建系统
├── img_test/      # 测试图片素材
│   └── 曲奇/      #   曲奇饼干检测样例（29 张 .jpg）
├── output/        # 运行时输出（程序自动生成，路径由 parameters.json 配置）
│   ├── detImg/    #   检测结果图（按时间戳分目录）
│   ├── log/       #   运行日志（.txt，每时间戳一份）
│   └── saveImg/   #   相机采集原图
└── yolomodel/     # 模型与标签
    ├── classes.txt     # 类别：0=物料，1=盒子
    ├── 曲奇.rknn       # 曲奇检测 RKNN 模型
    └── 藏药.rknn       # 藏药检测 RKNN 模型
```

| 目录 | 作用 |
|------|------|
| `RK3588/` | 全部源代码、构建脚本、配置、文档，独立 git 仓库 |
| `img_test/` | 离线测试样本图片（无相机环境调试用） |
| `output/` | 运行时生成：检测图、日志、采集原图（root 创建） |
| `yolomodel/` | RKNN 模型文件（NPU 专用）+ 类别标签 |

---

## 三、核心目录 `RK3588/` 详细结构

```
RK3588/
├── AGENTS.md                    # 项目说明文档（构建/结构/约定）
├── CMakeLists.txt               # 顶层 CMake 构建文件（主入口）
├── build.sh                     # 一键构建脚本
├── lv_conf.h                    # LVGL 配置（色深32、字体、FreeType 等，~700 行）
├── lv_drv_conf.h                # LVGL 驱动配置
├── yolov8_sam_demo.cpp          # ★ 主程序入口（354 行）
├── camera_demo.cpp              # USB 摄像头演示（CMake 中已注释禁用）
├── imagefile_demo.cpp           # 单图检测演示（已禁用）
├── videofile_demo.cpp           # 视频检测演示（已禁用）
│
├── include/                     # 公共头文件（RKNN API + 算法接口）
│   ├── rknn_api.h               #   Rockchip NPU runtime API
│   ├── rknn_matmul_api.h        #   RKNN 矩阵乘 API
│   ├── common.h                 #   通用数据结构（检测结果、bbox、模型类型枚举）
│   ├── yolov8.h                 #   Yolov8 推理类封装
│   ├── rknn_pool.h              #   RKNN 多线程推理池（核心并发组件）
│   ├── threadpool.h             #   通用 C++ 线程池
│   ├── image_process.h          #   图像前后处理（letterbox/画框）
│   ├── postprocess.h            #   YOLO 后处理（NMS 等）
│   ├── camera.h                 #   USB/V4L2 摄像头类
│   ├── videofile.h              #   视频文件读取类
│   └── Float16.h                #   半精度浮点工具
│
├── core/                        # ★ 应用层模块（业务逻辑）
│   ├── app_config.h             #   UI 运行期参数结构 AppConfig
│   ├── config.h / config.cpp    #   ★ JSON 配置系统（热加载/自动保存，全局单例 config::g）
│   ├── judgment.h / judgment.cpp#   ★ 满料判定逻辑（多盒、双竖线检测区）
│   ├── frame_bus.h / frame_bus.cpp  # 线程安全帧信箱（推理线程→UI 线程）
│   ├── camera_capture.h/.cpp    #   ★ OPT 工业相机采集（SciCamSDK）
│   ├── can_bus.h / can_bus.cpp  #   ★ CAN 总线结果发送
│   ├── gpio_out.h / gpio_out.cpp#   ★ GPIO 合格信号输出（TCA6424）
│   ├── gpio_in.h / gpio_in.cpp  #   GPIO 输入（暂停/恢复控制）
│   ├── gpio_utils.h / gpio_utils.cpp # TCA6424 引脚号→sysfs 编号换算
│   ├── logger.h / logger.cpp    #   spdlog 日志（控制台+文件）
│   └── ui_log.h / ui_log.cpp    #   UI 日志缓冲（info / warn+err 双通道）
│
├── ui/                          # ★ LVGL 图形界面层
│   ├── CMakeLists.txt           #   UI 子构建（ui_lib 静态库 + ui_test）
│   ├── ui_main.h / ui_main.cpp  #   LVGL/SDL 初始化、中文字体加载、tick 驱动
│   ├── ui_app.h / ui_app.cpp    #   ★ 主界面（~1000 行）：布局、推理工作线程、判定显示
│   ├── ui_canvas.h / ui_canvas.cpp       # 画布组件（cv::Mat→LVGL 显示）
│   ├── ui_filebrowser.h / ui_filebrowser.cpp # 模态文件浏览器（选模型/标签/输入）
│   ├── ui_test.cpp              #   UI 通路验证小程序（独立可执行）
│   └── mouse_cursor_icon.c      #   鼠标光标图标数据
│
├── utils/                       # ★ 运行时组件（编译为 yolov8-utils 静态库）
│   ├── CMakeLists.txt           #   生成 yolov8-utils 库
│   ├── yolov8.cpp               #   Yolov8 类实现
│   ├── rknn_pool.cpp            #   RknnPool 多线程推理池实现
│   ├── postprocess.cpp          #   YOLO 后处理
│   ├── image_process.cpp        #   图像处理
│   ├── camera.cpp               #   V4L2 摄像头实现
│   ├── videofile.cpp            #   视频文件读取实现
│   ├── camera_aravis.c          #   ★ Aravis 原生相机采集守护进程（替代方案）
│   └── mobilesam/               #   MobileSAM 分割模型完整实现（Rockchip 官方移植）
│       ├── mobilesam.h/.cc      #     SAM 主类
│       ├── mobilesam_pool.h/.cc #     SAM 线程池
│       ├── mobilesam_postprocess.h/.cc  # SAM 后处理（mask 生成）
│       ├── preprocess.h/.cc     #     预处理
│       ├── rknn_mobilesam_utils.h/.cc   # SAM RKNN 工具
│       ├── mobilesam_common.h   #     SAM 数据结构
│       ├── image_utils.*        #     图像工具
│       ├── image_drawing.*      #     图像绘制
│       ├── file_utils.*         #     文件工具
│       ├── font.h               #     调试字体
│       ├── stb_image.h / stb_image_write.h  # stb 图像库
│       └── rga*.h / im2d*.h / RockchipRga.h ...  # RGA 硬件加速头文件
│
├── lib/                         # 预编译 Rockchip 私有库
│   ├── librknnrt.so             #   RKNN NPU 运行时（7.7 MB）
│   └── librga.so                #   RGA 2D 图形加速（196 KB）
│
├── config/                      # 运行时配置（gitignored）
│   └── parameters.json          #   ★ 全局运行参数
│
├── scripts/                     # 部署/配置脚本
│   ├── install_sdk.sh           #   安装 SciCamSDK 到系统
│   └── setup_camera.sh          #   ★ OPT 相机一键配置（IP 发现 + SDK 安装）
│
├── docs/
│   └── camera_setup.md          #   ★ OPT 工业相机配置详尽指南（~286 行）
│
├── outputs/                     # 默认输出目录（CLI 模式用，当前空）
│
├── build/                       # 构建产物（gitignored）
│   ├── yolov8_sam_demo          #   ★ 主可执行程序（13.8 MB）
│   ├── camera_aravis            #   Aravis 相机守护进程
│   ├── ui/ui_test               #   UI 测试程序（1.5 MB）
│   ├── lib/                     #   liblvgl.a, liblv_drivers_lib.a
│   ├── ui/libui_lib.a           #   UI 库
│   ├── utils/libyolov8-utils.a  #   工具库
│   └── lvgl_build/              #   LVGL 构建中间产物
│
└── .gitignore
```

---

## 四、构建系统

### 4.1 顶层 `CMakeLists.txt` 要点

- **项目名**：`yolov8`，C++17 / C99，`Release`，`-O3 -g`
- **链接库目录**：`${CMAKE_SOURCE_DIR}/lib`（rknnrt、rga）
- **OpenCV**：`find_package(OpenCV REQUIRED)`
- **SciCamSDK**：硬编码 `/home/forlinx/SciCamSDK_V1.6.1.5_20250925/...`（OPT 相机 ARM64 SDK）
- **子目录**：`add_subdirectory(utils)`（生成 `yolov8-utils` 库）
- **LVGL UI 集成**（`option(ENABLE_UI ON)`）：
  - 外部 LVGL 源码：`LVGL_SRC_DIR=/home/ubuntu/lvgl-release-v8.3`、`lv_drivers`
  - 后端可选 `SDL`（开发）或 `FBDEV`（量产 framebuffer）
  - 依赖 **SDL2、FreeType（中文）、spdlog**
  - 构建 `lvgl`、`lv_drivers_lib`、`ui_lib` 三个静态库
- **可执行目标**：
  - `yolov8_sam_demo`（主程序）— 链接 OpenCV + yolov8-utils；UI 启用时再链接 ui_lib + lvgl + spdlog，并定义 `WITH_UI=1`
  - `camera_demo` / `imagefile_demo` / `videofile_demo`（**已注释，不构建**）

### 4.2 `build.sh` 关键环境变量

```bash
LVGL_SRC="${LVGL_SRC:-/home/forlinx/lvgl-release-v8.3}"
LV_DRIVERS_PARENT="${LV_DRIVERS_PARENT:-/home/forlinx/lv_port_linux-release-v8.3}"
LVGL_BACKEND="${LVGL_BACKEND:-SDL}"      # SDL 或 FBDEV
BUILD_TYPE="${BUILD_TYPE:-Release}"
```

执行 `cmake -B build ...` + `make -j$(nproc)`。

### 4.3 构建与运行

```bash
# 1. 构建
cd /home/forlinx/lvgl/RK3588
./build.sh                      # 或手动 cmake + make

# 2. 配置相机（首次，需 root）
sudo ./scripts/setup_camera.sh  # 网口检查→IP 配置→发现相机→装 SDK→验证

# 3. 运行（相机模式需 root，GigE Vision 要 raw socket）
sudo ./build/yolov8_sam_demo    # 默认 UI 模式
# 无头 CLI：
./build/yolov8_sam_demo /home/forlinx/lvgl/img_test/曲奇 --headless

# 4. UI 通路验证
./build/ui/ui_test [图片路径]
```

---

## 五、关键源码摘要

### 5.1 主入口 `yolov8_sam_demo.cpp`（354 行）

两种运行模式（编译期 `WITH_UI` 宏 + 运行期参数决定）：

1. **UI 模式（默认）**
   - `config::init(...)` 加载配置
   - 初始化 logger、CAN 总线、GPIO 输出
   - 启动 GPIO 输入轮询线程（P27=HIGH 暂停，LOW 恢复）
   - 进入 `run_ui_mode()`

2. **CLI 无头模式**（`--headless`）
   - 创建 `RknnPool`（YOLO 多线程池，默认 3 线程）
   - 可选 `MobileSamPool`（`--no-sam` 关闭）
   - 输入：单图 / 目录（批量）/ 视频流
   - 推理：提交任务 → 取结果 → （可选）SAM 分割 → 保存
   - 满料判定 + CAN 发送：`judge_all_boxes()` → `summarize()` → `is_qualified()` → `can_bus::send_result(ok)` + `gpio_out::set_qualified(ok)`
   - 结束时打印性能统计（YOLO/SAM 各阶段耗时、平均 FPS）

### 5.2 UI 主逻辑 `ui/ui_app.cpp`（~1000 行）

`run_ui_mode()` 总入口：

- **界面布局**（1280×720）
  - 顶部菜单栏（48px）：YOLO 模型 / 标签文件 / 输入源 / 开始 / 停止 / 采集图像 / 退出
  - 左侧：实时画面 canvas（820×461）+ 两条可拖动竖线（检测区 L/R）+ 物料信息文本框
  - 右侧面板（460px）：生产统计（正确/错误计数、当前帧判定）、目标物料数 ±、文件信息、日志视图（运行日志 / 警告报警切换）

- **推理工作线程 `worker_fn()`**
  - 常驻 YOLO 模型池（不每次重建）
  - 严格串行：提交一帧 → 等结果 → 显示 → 再提交下一帧
  - 支持摄像头实时、单图、目录批量、视频
  - 每帧画检测框 + 双竖线 + 盒子判定（FULL / NOT FULL / NOT REVEALED），保存到 `detImg_root/<时间戳>/`
  - 合格判定 → CAN + GPIO 输出

- **判定核心**（`judgment.h`）
  - `judge_all_boxes()`：找出所有盒子，判断是否在两竖线内（revealed 完全漏出），物料按中心点归属盒子
  - `is_qualified()`：**检测区内有满料盒且无未满盒 → 合格**
  - UI 拖动竖线时实时重判（`run_judgment`）

- **配置热加载**：主循环 `config::poll_hot_reload()` 检测 mtime 自动重载；UI 改动 `mark_dirty()` 后 1s 防抖回写

### 5.3 配置系统 `core/config.h`

`Config` 结构体主要字段（默认值见 `config.h`，实际值见 `parameters.json`）：

| 分组 | 字段 | 当前值 | 说明 |
|------|------|--------|------|
| 判定 | `material_class` | 0 | 物料类别 ID |
| | `box_class` | 1 | 盒子类别 ID |
| | `target_count` | 4 | 满料目标物料数 |
| | `line_left_frac` | 0.3 | 左竖线占图宽比例 |
| | `line_right_frac` | 0.894 | 右竖线占图宽比例 |
| 模型/路径 | `yolo_model` | `/home/forlinx/lvgl/yolomodel/曲奇.rknn` | YOLO 模型 |
| | `label_path` | `/home/forlinx/lvgl/yolomodel/classes.txt` | 标签 |
| | `sam_encoder/decoder` | `model/mobile_sam_*.rknn` | SAM 模型 |
| | `detImg_root` | `/home/forlinx/lvgl/output/detImg` | 检测图根目录 |
| | `saveImg_root` | `/home/forlinx/lvgl/output/saveImg` | 采集图根目录 |
| | `log_root` | `/home/forlinx/lvgl/output/log` | 日志根目录 |
| | `default_input` | `/home/forlinx/lvgl/img_test/曲奇` | 默认输入 |
| | `use_sam` | false | 是否启用 SAM 分割 |
| CAN | `can_enabled` | true | 启用 CAN 结果发送 |
| | `can_send_if/recv_if` | can0 / can1 | 收发接口 |
| | `can_id` | 768 (0x300) | CAN ID |
| GPIO | `gpio_enabled` | true | GPIO 合格输出（P23） |
| | `gpio_input_enabled` | true | GPIO 暂停输入（P27） |
| 相机 | `camera_enabled` | true | 启用工业相机 |
| | `camera_iface` | eth1 | 相机网口 |
| | `camera_ip` | 169.254.100.100/16 | 开发板 IP |
| | `camera_grab_bin` | `build/camera_aravis` | 采集守护进程 |
| UI | `ui_width/height` | 1280/720 | 窗口尺寸 |

特性：自定义扁平 JSON 解析、`poll_hot_reload()` 热加载、`mark_dirty()`/`poll_save_due()` 1s 防抖自动保存、全局单例 `config::g`。

### 5.4 相机采集 `core/camera_capture.cpp`

直接调用 **OPT SciCamSDK** 原生 ARM64 驱动：配置 eth1 IP → `SciCam_DiscoveryDevices` → `CreateDevice/OpenDevice` → 自动曝光增益 → `StartGrabbing`。`grab()` 取帧并 Mono8→BGR 转换。**GigE Vision 需 raw socket，必须 `sudo`。**

备选方案 `utils/camera_aravis.c`：用 Aravis 开源库，输出共享内存 `/tmp/camera_frame.raw` + `/tmp/camera_info.txt`。

---

## 六、技术栈与依赖

### 编译/运行依赖（须预装）

| 依赖 | 用途 |
|------|------|
| OpenCV | 图像 IO、颜色转换、画框、视频读取 |
| SDL2 | LVGL 显示/输入后端（开发模式） |
| FreeType | LVGL 中文字体渲染（NotoSansCJK） |
| spdlog + fmt | 日志（控制台 + 文件双 sink） |
| CMake ≥ 3.16 | 构建系统 |
| Aravis（可选） | `camera_aravis.c` 备选相机方案 |

### 预编译私有库（`lib/`，不可重建）

| 库 | 作用 |
|----|------|
| `librknnrt.so` (7.7 MB) | Rockchip NPU 推理运行时 |
| `librga.so` (196 KB) | Rockchip 2D 图形硬件加速（RGA） |

### 外部源码（预装于构建机）

- LVGL v8.3（`/home/forlinx/lvgl-release-v8.3`）
- lv_port_linux + lv_drivers（`/home/forlinx/lv_port_linux-release-v8.3`）
- OPT SciCamSDK V1.6.1.5（`/home/forlinx/SciCamSDK_...`）

### 语言/标准

C++17、C99、CMake、Bash、C（camera_aravis / lv_drivers）

---

## 七、模型与资源

### 模型（`yolomodel/`）

| 文件 | 类型 | 说明 |
|------|------|------|
| `曲奇.rknn` | RKNN | 曲奇饼干检测模型（当前使用） |
| `藏药.rknn` | RKNN | 藏药检测模型（另一任务） |
| `classes.txt` | 文本 | 类别：0（物料）、1（盒子），共 2 类 |

> RKNN = Rockchip NPU 专用格式，由 ONNX/PyTorch 转换，运行于 RK3588 的 6 TOPS NPU。

### SAM 模型（路径见 config）

`model/mobile_sam_encoder.rknn`、`model/mobile_sam_decoder.rknn`（当前 `use_sam=false`）

### 测试图片（`img_test/曲奇/`）

29 张 `.jpg`（约 500KB/张），命名含序号和参数，曲奇包装盒的工业相机实拍图。

### 字体

`NotoSansCJK-Regular.ttc`（FreeType 动态加载，22px 常规 + 16px 小号）

---

## 八、构建产物（`build/`，已编译）

| 产物 | 大小 | 说明 |
|------|------|------|
| `yolov8_sam_demo` | 13.8 MB | **主可执行程序**（UI + CLI 双模式） |
| `ui/ui_test` | 1.5 MB | UI 通路验证程序 |
| `camera_aravis` | 18 KB | Aravis 相机守护进程 |
| `lib/liblvgl.a` | — | LVGL 静态库 |
| `lib/liblv_drivers_lib.a` | — | LVGL 驱动库 |
| `ui/libui_lib.a` | — | UI 应用库 |
| `utils/libyolov8-utils.a` | — | 算法工具库 |

---

## 九、运行时输出（`output/`，已有历史数据）

- `detImg/<时间戳>/000XX.jpg`：带框检测结果图（按时间戳归档）
- `log/<时间戳>.txt`：spdlog 文件日志（40+ 个历史日志）
- `saveImg/<时间戳>/000000.jpg`：相机采集原图（"采集图像"按钮触发）

---

## 十、架构亮点

1. **多线程 RKNN 推理池**（`rknn_pool` + `threadpool`）充分利用 RK3588 三核 NPU
2. **生产者-消费者解耦**（`frame_bus` 信箱）分离推理线程与 UI 线程
3. **JSON 配置热加载**支持不停机调参
4. **双竖线检测区 + 多盒判定**的灵活业务逻辑
5. **多输入源统一抽象**（工业相机 / USB 相机 / 图片 / 目录 / 视频）
6. **可裁剪**：SAM、CAN、GPIO、相机均可通过配置开关禁用

---

## 十一、一句话总结

一套部署在 **RK3588 + OPT 工业相机**上的**工业产线视觉检测系统**，用 YOLOv8（NPU 加速）实时检测包装盒内物料数量，判断满料合格性，通过 CAN 总线和 GPIO 与产线 PLC 联动，并配 LVGL 触屏界面供现场操作。
