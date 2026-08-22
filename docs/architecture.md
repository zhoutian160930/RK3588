# 代码结构与新增功能指南

> 面向后续维护:想加功能时先看第 2 节"决策表",按功能类型找到对应位置和模板。

## 1. 分层架构(为什么这样分)

```
┌────────────────────────────────────────────────────┐
│ qtui/          界面层(Qt)                          │
│   只通过 pipeline.h 的接口/atomic/FrameBus 与业务层 │
│   交互,不直接碰推理栈和相机 SDK                     │
├────────────────────────────────────────────────────┤
│ core/          业务层(UI 无关,可脱离界面测试)      │
│   pipeline: 推理 worker(输入→推理→判定→发布)      │
│   judgment:  判定纯函数                             │
│   camera_capture / can_bus / gpio_* / image_saver   │
│   config:   全局配置(JSON 热加载)                  │
├────────────────────────────────────────────────────┤
│ utils/         推理栈(yolov8-utils,几乎不用动)    │
│   rknn_pool / yolov8 / postprocess / image_process  │
│   (RGA 预处理) / mobilesam                          │
└────────────────────────────────────────────────────┘
```

三条铁律:
1. **core/ 不 include 任何 Qt 头**——判定/硬件/相机逻辑必须能在 headless 版跑
2. **UI 与 worker 只通过 `pipeline.h` 暴露的 atomic + FrameBus 通信**,不共享普通变量
3. **判定逻辑(judgment)是纯函数**:输入检测结果+参数,输出结论,不改任何全局状态

## 2. 新增功能决策表

| 想加什么 | 去哪儿 | 怎么加(模板) |
|---|---|---|
| **改判定规则**(满料标准/物料归属/检测区) | `core/judgment.{h,cpp}` | 纯函数,改完编译即生效;UI 拖线重判和 worker 判定自动同步(共用) |
| **新输出通道**(串口/TCP/MQTT 上报结果) | 新建 `core/xxx_bus.{h,cpp}` | 照抄 `can_bus` 三件套:init/send_result/shutdown;`qt_main.cpp:17-26` 处 init;`pipeline.cpp:190` 旁调用 send |
| **界面新按钮/面板** | `qtui/mainwindow.cpp` 工具栏 + 新建面板文件 | 面板照抄 `stats_panel` (h+cpp),在 `buildDocks()` 挂 Dock;别忘了 CMake 源文件列表 |
| **新配置项**(可配参数) | `core/config.h` 字段 + `config.cpp` 两处(parse_kv/save 各一行) | 见第 3 节完整清单 |
| **相机新参数**(如触发模式/白平衡) | `core/camera_capture.cpp` | 照 `apply_exposure_locked()` 模式:读范围→钳位→Set;需热加载则导出 apply_xxx() 并在 `mainwindow.cpp:429 pollConfig()` 挂钩 |
| **改存图策略**(何时存/存什么/格式) | `core/pipeline.cpp` 的 `drain_one`(244 行附近) | 存图走 `image_saver::enqueue`(异步);判定发布/CAN/GPIO 也都在这个函数 |
| **新输入源**(网络流/多相机) | `core/pipeline.cpp` 的 `worker_fn` 输入分支(相机/图/目录/视频四选一处) | 加一个 else if 分支,照现有模式的 process_one 节拍 |
| **换模型/改预处理** | `utils/`(rknn_pool/yolov8/image_process) | 注意 Convert(src,to_rgb) 返回的 Mat 引用线程缓冲,下次调用前须用完 |
| **启动时初始化新模块** | `qtui/qt_main.cpp`(顺序:config→logger→CAN→GPIO→GPIO轮询→UI) | 有 atexit 清理的模块记得 shutdown 对应加到 mainwindow closeEvent |

## 3. 加一个配置项的完整清单(最常做)

以"新增 xxx 参数"为例,共 4 处 + 文档:

```cpp
// ① core/config.h —— struct Config 加字段(带默认值和注释)
int xxx = 0;

// ② core/config.cpp apply_kv() —— 解析(照相邻行)
else if (k == "xxx") g.xxx = std::atoi(v.c_str());   // 字符串/浮点用 atof/直接赋值

// ③ core/config.cpp write_file() —— 保存(JSON 尾部,注意最后一项无逗号)
out << "  \"xxx\": " << g.xxx << "\n";

// ④ 使用处读 config::g.xxx(热加载自动生效;UI 改则 config::mark_dirty())
```

```
⑤ docs/deployment.md 第 5 节配置表加一行说明
```

> 热加载语义:程序每 500ms 检查 parameters.json mtime,变化即重载(pipeline 运行中暂停重载,
> 除曝光/增益这类显式挂钩的项)。

## 4. 数据流(排查问题按这条链走)

```
camera_capture::grab() ──BGR 帧──▶ pipeline worker_fn
    │ process_one: 提交 RknnPool(NPU×3线程)
    │ drain_one: 取结果 → judgment 判定 ──qualified──▶ CAN(pipeline.cpp:190)
    │                                            ├──▶ GPIO(pipeline.cpp:191)
    │   画框/画线 → image_saver(异步存图)
    │            → FrameBus.push(最新帧信箱)
    ▼
UI QTimer 轮询(mainwindow.cpp):
    pollFrame(5ms)   取帧显示+物料信息
    pollAux(150ms)   日志/计数/拖线重判/worker回收
    pollConfig(500ms) 热加载+防抖保存+曝光下发
    pollCamera(60ms)  空闲预览+采集存图
```

## 5. 常用验证方法

| 改动 | 验证 |
|---|---|
| judgment 判定 | headless 跑图集:`build/yolov8_sam_demo <图片目录> --headless`,看日志 boxes/full/notfull |
| core 新模块 | 先写独立小测试(g++ 单文件 + core/xxx.cpp),过了再进工程 |
| 界面 | `--platform offscreen` 无头起 Qt 版,看 spdlog 日志 |
| 预处理/推理 | 对比日志 `frame N timing: pre/infer/draw/uiwait`;`RKNN_FORCE_CPU_PREPROCESS=1` A/B |
