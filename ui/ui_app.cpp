#include "ui_app.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "frame_bus.h"
#include "image_process.h"
#include "logger.h"
#include "config.h"
#include "can_bus.h"
#include "gpio_out.h"
#include "gpio_in.h"
#include "camera_capture.h"
#include "judgment.h"
#include "ui_log.h"
#include "mobilesam/mobilesam_pool.h"
#include "rknn_pool.h"
#include "ui_canvas.h"
#include "ui_filebrowser.h"
#include "ui_main.h"

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

/* detImg/log 根目录由 config.json 决定(config::g.detImg_root / log_root) */

namespace {
/* ---- 布局常量 ---- */
constexpr int WIN_W = 1280, WIN_H = 720;
constexpr int TOP_H = 48;              /* 顶部菜单按钮栏(标题用 SDL 窗口标题) */
constexpr int PANEL_W = 460;           /* 右侧面板宽 */
constexpr int LEFT_W = WIN_W - PANEL_W;/* 820 */
constexpr int CV_W = LEFT_W;           /* canvas 宽 */
constexpr int CV_H = LEFT_W * 9 / 16;  /* 461 */
constexpr int CV_Y = 0;                /* canvas 在 left 容器里的 y */

/* ---- 运行期状态 ---- */
AppConfig g_cfg;
std::string g_input_path;
std::string g_yolo_path;
std::string g_label_path;
std::string g_sam_enc_path;
std::string g_sam_dec_path;
bool g_use_sam = false;
int g_yolo_threads = 3;
bool g_use_camera = false;  /* 摄像头输入模式 */


/* 两条竖线(检测区左右边界)，单位为 canvas 坐标 [0, CV_W]；atomic 供 worker 实时读 */
std::atomic<int> g_line_left_x{0};
std::atomic<int> g_line_right_x{0};
lv_obj_t *g_line_left = nullptr;
lv_obj_t *g_line_right = nullptr;
std::atomic<int> g_target_count{10};          /* 目标物料数(UI 与 worker 共享，从 config 初始化) */
std::atomic<int> g_material_class{0};         /* 物料类别 ID(config) */
std::atomic<int> g_box_class{1};              /* 盒子类别 ID(config) */
/* 运行期累计：正确(盒内物料数==目标且完全漏出)/错误(否则) */
std::atomic<int> g_correct_count{0};
std::atomic<int> g_wrong_count{0};

FramePayload g_last_payload;                  /* 最近一帧(用于拖线时实时重判) */
bool g_has_last = false;

FrameBus g_bus;
std::unique_ptr<RknnPool> g_yolo_pool;  /* 常驻 YOLO 模型(不每轮重建) */
std::thread g_worker;
std::atomic<bool> g_stop{false};
enum UiState { ST_IDLE = 0, ST_RUNNING = 1, ST_DONE = 2, ST_STOPPED = 3 };
std::atomic<int> g_state{ST_IDLE};
std::atomic<int> g_proc_count{0};

/* ---- 控件句柄 ---- */
lv_obj_t *g_canvas;
lv_obj_t *g_input_label, *g_yolo_label, *g_label_label;
lv_obj_t *g_status_label;
lv_obj_t *g_judge_label;   /* 满料判定结果(大字) */
lv_obj_t *g_stat_label;    /* 累计 正确/错误 物料数 */
lv_obj_t *g_start_btn, *g_stop_btn;
lv_obj_t *g_quit_btn;
lv_obj_t *g_target_label;  /* 目标物料数(显示值) */
lv_obj_t *g_info_box = nullptr;   /* 左下：当前图片物料信息(滚动) */
lv_obj_t *g_log_view = nullptr;  /* 右下：日志视图(单栏，按按钮切换内容) */
int g_log_mode = 0;              /* 0=运行日志(info) 1=警告和报警(warn+err) */

std::string base_name(const std::string &p) {
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}

/* 去除路径尾部空白/斜杠等隐藏字符 */
std::string trim_path(const std::string &p) {
  std::string r = p;
  while (!r.empty()) {
    char c = r.back();
    if (c == '/' || c == ' ' || c == '\n' || c == '\r' || c == '\t')
      r.pop_back();
    else
      break;
  }
  return r;
}

void update_status_text() {
  const char *st = "待机";
  switch (g_state.load()) {
    case ST_RUNNING: st = "运行中"; break;
    case ST_DONE: st = "完成"; break;
    case ST_STOPPED: st = "已停止"; break;
    default: st = "待机"; break;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "状态: %s  | 已处理: %d", st, g_proc_count.load());
  lv_label_set_text(g_status_label, buf);
}

/* ---- 文件浏览器回调 ---- */
void on_fb_input(const char *path, void *) {
  g_input_path = trim_path(path);
  config::g.default_input = g_input_path;
  config::mark_dirty();
  if (g_input_label) lv_label_set_text(g_input_label, base_name(path).c_str());
  SPDLOG_INFO("输入源已选择: {}", g_input_path);
}
void on_fb_yolo(const char *path, void *) {
  g_yolo_path = trim_path(path);
  config::g.yolo_model = g_yolo_path;
  config::mark_dirty();
  if (g_yolo_label) lv_label_set_text(g_yolo_label, base_name(path).c_str());
  SPDLOG_INFO("YOLO 模型已切换: {}", g_yolo_path);
}
void on_fb_label(const char *path, void *) {
  g_label_path = trim_path(path);
  config::g.label_path = g_label_path;
  config::mark_dirty();
  if (g_label_label) lv_label_set_text(g_label_label, base_name(path).c_str());
  SPDLOG_INFO("标签文件已切换: {}", g_label_path);
}

/* 输入源二选一弹出菜单：本地文件夹 / 摄像头 */
static lv_obj_t *g_src_popup = nullptr;
static void close_src_popup() {
  if (g_src_popup) { lv_obj_del(g_src_popup); g_src_popup = nullptr; }
}
static void on_src_folder(lv_event_t *) {
  close_src_popup();
  g_use_camera = false;
  camera_capture::shutdown();  /* 切回文件夹时关闭相机 */
  ui_filebrowser_open(config::g.fb_input_dir.c_str(),
                      ".jpg,.jpeg,.png,.bmp,.mp4,.avi,.mkv", 1, on_fb_input, NULL);
}
static void on_src_camera(lv_event_t *) {
  close_src_popup();
  g_use_camera = true;
  camera_capture::init(0);  /* 立刻加载相机(已就绪则幂等返回) */
  SPDLOG_INFO("输入源：摄像头");
  if (g_input_label) lv_label_set_text(g_input_label, "摄像头");
}
void open_input_source_menu(lv_event_t *) {
  close_src_popup();
  g_src_popup = lv_obj_create(lv_scr_act());
  lv_obj_set_size(g_src_popup, 220, 130);
  lv_obj_center(g_src_popup);
  lv_obj_set_style_bg_color(g_src_popup, lv_color_white(), 0);
  lv_obj_set_style_border_width(g_src_popup, 2, 0);
  lv_obj_clear_flag(g_src_popup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *tt = lv_label_create(g_src_popup);
  lv_label_set_text(tt, "选择输入源");
  lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_t *b1 = lv_btn_create(g_src_popup);
  lv_obj_set_size(b1, 180, 36);
  lv_obj_align(b1, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_t *l1 = lv_label_create(b1);
  lv_label_set_text(l1, "本地文件夹");
  lv_obj_center(l1);
  lv_obj_add_event_cb(b1, on_src_folder, LV_EVENT_CLICKED, NULL);
  lv_obj_t *b2 = lv_btn_create(g_src_popup);
  lv_obj_set_size(b2, 180, 36);
  lv_obj_align(b2, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_t *l2 = lv_label_create(b2);
  lv_label_set_text(l2, "摄像头(预留)");
  lv_obj_center(l2);
  lv_obj_add_event_cb(b2, on_src_camera, LV_EVENT_CLICKED, NULL);
}

void open_yolo_browser(lv_event_t *) {
  ui_filebrowser_open(config::g.fb_model_dir.c_str(), ".rknn", 0, on_fb_yolo, NULL);
}
void open_label_browser(lv_event_t *) {
  ui_filebrowser_open(config::g.fb_model_dir.c_str(), ".txt", 0, on_fb_label, NULL);
}

/* ---- 推理工作线程 ---- */
void worker_fn() {
  g_state = ST_RUNNING;
  g_proc_count = 0;
  g_correct_count = 0;
  g_wrong_count = 0;
  g_stop = false;
  AppConfig cfg = g_cfg;

  /* 诊断：打印路径长度（便于发现尾部隐藏字符） */
  SPDLOG_DEBUG("worker start: raw input_path='{}' len={}", cfg.input_path,
               cfg.input_path.size());
  /* 去除尾部的空白/斜杠（修掉"目录判定失败"的常见原因） */
  while (!cfg.input_path.empty()) {
    char c = cfg.input_path.back();
    if (c == '/' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      cfg.input_path.pop_back();
    } else {
      break;
    }
  }
  SPDLOG_DEBUG("worker: trimmed input_path='{}' len={}", cfg.input_path,
               cfg.input_path.size());

  SPDLOG_INFO("loading YOLO model: {} (threads={})", cfg.yolo_path,
              cfg.yolo_threads);
  if (!g_yolo_pool) {
    RknnPool *p = new RknnPool(cfg.yolo_path, cfg.yolo_threads, cfg.label_path);
    g_yolo_pool.reset(p);
    SPDLOG_INFO("YOLO model loaded (常驻)");
  }
  RknnPool &yolo = *g_yolo_pool;
  /* 清空上次残余结果(停止时可能未排空) */
  while (yolo.GetImageResultFromQueue().img != nullptr) {
  }
  std::unique_ptr<MobileSamPool> sam;
  if (cfg.use_sam && !cfg.sam_enc_path.empty() && !cfg.sam_dec_path.empty()) {
    SPDLOG_INFO("init SAM: {} / {}", cfg.sam_enc_path, cfg.sam_dec_path);
    sam = std::make_unique<MobileSamPool>(cfg.sam_enc_path, cfg.sam_dec_path,
                                          cfg.sam_threads);
    if (sam->Init() != 0) {
      SPDLOG_ERROR("SAM init failed, SAM disabled");
      sam.reset();
    }
  }

  /* 先判目录，再判图片，最后判视频；避免对目录做无意义的 imread */
  bool is_dir = fs::is_directory(cfg.input_path);
  bool is_image = false;
  cv::Mat img0;
  if (!is_dir) {
    img0 = cv::imread(cfg.input_path);
    is_image = !img0.empty();
  }
  cv::VideoCapture cap;
  bool is_video = false;
  if (!is_dir && !is_image) {
    cap.open(cfg.input_path);
    is_video = cap.isOpened();
  }
  SPDLOG_DEBUG("input kind: is_dir={} is_image={} is_video={}", is_dir, is_image,
               is_video);
  if (!is_dir && !is_image && !is_video) {
    SPDLOG_ERROR("invalid input (not dir/image/video): {}", cfg.input_path);
    g_state = ST_STOPPED;
    g_bus.set_done();
    return;
  }

  /* 本次运行的结果保存目录：detImg_root/<时间戳>/（绝对路径） */
  std::time_t now_t = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now_t));
  std::string save_dir = config::g.detImg_root + "/" + ts;
  fs::create_directories(save_dir);
  SPDLOG_INFO("save_dir={}", save_dir);

  int submitted = 0;
  auto stopped = [&] { return g_stop.load(); };

  /* 取一个结果，画框后 push 到 bus，并等 UI 把它取走显示。
   * 这是"推理一帧展示一帧"的关键：每帧必须被 UI pop 走，才继续下一帧。 */
  auto drain_one = [&] {
    YoloResult r = yolo.GetImageResultFromQueue();
    if (!r.img) return false;
    cv::Mat out = r.img->clone();
    ImageProcess ip(out.cols, out.rows, 640);
    ip.ImagePostProcess(out, r.results);

    /* 画两条竖线 + 每个盒子的判定文字到图上(保存与显示都用) */
    float llf = g_line_left_x.load() / (float)CV_W;
    float rrf = g_line_right_x.load() / (float)CV_W;
    int target = g_target_count.load();
    int bc = g_box_class.load(), mc = g_material_class.load();
    auto boxes = judge_all_boxes(r.results, out.cols, llf, rrf, target, bc, mc);
    JudgeSummary sum = summarize(boxes);
    /* 本帧合格判定(唯一来源)：检测区内有满料盒且无未满盒 → 合格。
     * 区外未漏出的盒子不计入。下面的统计/CAN/日志全部用这个结果。 */
    bool qualified = is_qualified(sum);
    /* 累计正确/错误(按帧)：合格帧计入正确，不合格帧计入错误 */
    if (sum.total > 0) {
      if (qualified)
        g_correct_count.fetch_add(1);
      else
        g_wrong_count.fetch_add(1);
    }
    /* CAN 直接发送合格判定结果，不再二次判断 */
    if (config::g.can_enabled) can_bus::send_result(qualified);
    gpio_out::set_qualified(qualified);
    SPDLOG_INFO("frame {} 合格={}", g_proc_count.load(), qualified);
    int ll = (int)(llf * out.cols), rr = (int)(rrf * out.cols);
    cv::line(out, cv::Point(ll, 0), cv::Point(ll, out.rows),
             cv::Scalar(255, 255, 0), 2);  /* 左线 青 */
    cv::line(out, cv::Point(rr, 0), cv::Point(rr, out.rows),
             cv::Scalar(255, 0, 255), 2);  /* 右线 品红 */
    /* 顶部汇总 */
    char top[128];
    snprintf(top, sizeof(top), "Boxes: %d | FULL %d  NOTFULL %d  NOTREVEALED %d",
             sum.total, sum.full, sum.not_full, sum.not_revealed);
    cv::Scalar top_col = (sum.not_full == 0 && sum.not_revealed == 0 && sum.total > 0)
                             ? cv::Scalar(0, 255, 0)
                             : cv::Scalar(0, 165, 255);
    cv::putText(out, top, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 0.9,
                top_col, 2);
    /* 每个盒子在其上方画判定 */
    for (auto &b : boxes) {
      const char *tag;
      cv::Scalar col;
      char buf[64];
      if (!b.revealed) {
        tag = "NOT REVEALED";
        col = cv::Scalar(0, 165, 255);
        snprintf(buf, sizeof(buf), "%s", tag);
      } else if (b.full) {
        col = cv::Scalar(0, 255, 0);
        snprintf(buf, sizeof(buf), "FULL %d/%d", b.material_count, target);
      } else {
        col = cv::Scalar(0, 0, 255);
        snprintf(buf, sizeof(buf), "NOT FULL %d/%d", b.material_count, target);
      }
      int y = b.box_top - 8 > 10 ? b.box_top - 8 : b.box_bottom + 22;
      cv::putText(out, buf, cv::Point(b.box_left, y), cv::FONT_HERSHEY_SIMPLEX,
                  0.8, col, 2);
    }

    FramePayload payload;
    payload.frame = out;
    payload.results = r.results;
    payload.orig_w = out.cols;
    payload.orig_h = out.rows;
    int cur_idx = g_proc_count.load();
    SPDLOG_DEBUG("push frame {} to UI", cur_idx);
    g_bus.push(payload);
    /* 等 UI 取走这一帧（最多 ~1s 保底，避免 UI 卡死拖死 worker） */
    for (int i = 0; i < 333 && g_bus.is_pending() && !stopped(); ++i) usleep(3000);

    /* 保存到时间戳子目录 */
    char savepath[512];
    snprintf(savepath, sizeof(savepath), "%s/%05d.jpg", save_dir.c_str(), cur_idx);
    bool ok = cv::imwrite(savepath, out);
    if (!ok) SPDLOG_ERROR("imwrite failed: {}", savepath);

    /* 每图评判结果日志(多盒汇总) */
    if (sum.total > 0) {
      SPDLOG_INFO(
          "frame {} | boxes={} full={} notfull={} notrevealed={} lines(frac)={:.2f}~{:.2f} "
          "target={} | save={}",
          cur_idx, sum.total, sum.full, sum.not_full, sum.not_revealed, llf, rrf,
          target, savepath);
    } else {
      SPDLOG_INFO("frame {} | no box(cls{}) detected | save={}", cur_idx, bc,
                  savepath);
    }

    if (sam) {
      std::vector<mobilesam_box> boxes;
      for (int i = 0; i < r.results.count; i++) {
        auto &d = r.results.results[i];
        boxes.push_back({d.box.left, d.box.top, d.box.right, d.box.bottom});
      }
      char nm[512];
      snprintf(nm, sizeof(nm), "%s/%05d_sam.jpg", save_dir.c_str(), cur_idx);
      sam->AddInferenceTask(*r.img, nm, boxes);
    }
    g_proc_count++;
    return true;
  };

  /* 严格串行：提交一张 -> 等它推理完并显示 -> 才提交下一张。
   * 这样保证"推理一张就展示一张"，而不是全部推理完再一起出。 */
  auto process_one = [&](std::shared_ptr<cv::Mat> src) {
    /* 外部暂停控制：P24=HIGH 时阻塞等待恢复 */
    while (gpio_in::is_system_paused() && !stopped()) {
      usleep(200000);
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    ImageProcess ip(src->cols, src->rows, 640);
    yolo.AddInferenceTask(src, ip, "");
    submitted++;
    SPDLOG_DEBUG("submit frame {}, {}x{}", submitted, src->cols, src->rows);
    /* 等这一帧的结果出现并显示完 */
    while (g_proc_count.load() < submitted && !stopped()) {
      drain_one();
      usleep(2000);
    }
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - t0)
                    .count();
    SPDLOG_INFO("frame {} total cost {:.1f} ms", submitted - 1, ms);
  };

  if (g_use_camera) {
    /* 摄像头已由 UI 层预先加载，这里直接抓帧推理。"停止"后不杀相机，继续后台运行 */
    if (!camera_capture::init(0)) {
      SPDLOG_ERROR("camera: 初始化失败，退出 worker");
    } else {
      SPDLOG_INFO("camera: 开始实时推理");
      int cam_frames = 0;
      while (!stopped()) {
        cv::Mat frame;
        if (camera_capture::grab(frame)) {
          cam_frames++;
          SPDLOG_DEBUG("camera: grab 帧 {}", cam_frames);
          process_one(std::make_shared<cv::Mat>(frame.clone()));
        } else {
          usleep(5000);
        }
      }
      SPDLOG_INFO("camera: 循环退出, 共处理 {} 帧, stopped={}", cam_frames,
                  stopped());
    }
  } else if (is_image) {
    process_one(std::make_shared<cv::Mat>(img0.clone()));
  } else if (is_dir) {
    std::vector<fs::path> files;
    for (auto &p : fs::directory_iterator(cfg.input_path)) {
      if (!p.is_regular_file()) continue;
      auto ext = p.path().extension().string();
      for (auto &c : ext) c = (char)std::tolower(c);
      if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
        files.push_back(p.path());
    }
    std::sort(files.begin(), files.end());
    for (auto &p : files) {
      if (stopped()) break;
      cv::Mat im = cv::imread(p.string());
      if (im.empty()) continue;
      process_one(std::make_shared<cv::Mat>(im));
    }
  } else {
    while (!stopped()) {
      cv::Mat frame;
      cap >> frame;
      if (frame.empty()) break;
      process_one(std::make_shared<cv::Mat>(frame.clone()));
    }
  }

  g_bus.set_done();
  g_state = stopped() ? ST_STOPPED : ST_DONE;
}

/* 竖线拖动回调：按鼠标位移移动竖线，限制在 [0, CV_W] */
void line_drag_cb(lv_event_t *e) {
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t vect;
  lv_indev_get_vect(indev, &vect);
  int x = lv_obj_get_x(obj) + vect.x;
  if (x < 0) x = 0;
  if (x > CV_W) x = CV_W;
  lv_obj_set_x(obj, x);
  if (obj == g_line_left) {
    g_line_left_x = x;
    config::g.line_left_frac = (float)x / CV_W;
  } else if (obj == g_line_right) {
    g_line_right_x = x;
    config::g.line_right_frac = (float)x / CV_W;
  }
  config::mark_dirty();
}

/* 创建一条可拖动竖线(8px 宽，全高，带顶部抓手标签) */
lv_obj_t *make_drag_line(lv_obj_t *parent, int x, lv_color_t color,
                         const char *tag) {
  lv_obj_t *line = lv_obj_create(parent);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, 8, CV_H);
  lv_obj_set_pos(line, x - 4, CV_Y);
  lv_obj_set_style_bg_color(line, color, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(line, 0, 0);
  lv_obj_add_flag(line, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(line, line_drag_cb, LV_EVENT_PRESSING, NULL);
  /* 顶部抓手小标签，便于拖动时抓取 */
  lv_obj_t *h = lv_label_create(line);
  lv_label_set_text(h, tag);
  lv_obj_set_style_bg_color(h, color, 0);
  lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(h, lv_color_black(), 0);
  lv_obj_set_style_pad_all(h, 2, 0);
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 0);
  return line;
}

void apply_resolution() {
  /* 固定 canvas 尺寸，不缩放；竖线直接覆盖在 canvas 上，坐标一致 */
  ui_canvas_resize(CV_W, CV_H);
  lv_img_set_zoom(g_canvas, 256);
  lv_obj_set_pos(g_canvas, 0, CV_Y);
  /* (重新)放置竖线 */
  if (!g_line_left)
    g_line_left = make_drag_line(g_canvas->parent, g_line_left_x,
                                 lv_color_make(0, 255, 255), "L");
  if (!g_line_right)
    g_line_right = make_drag_line(g_canvas->parent, g_line_right_x,
                                  lv_color_make(255, 0, 255), "R");
  lv_obj_set_pos(g_line_left, g_line_left_x - 4, CV_Y);
  lv_obj_set_pos(g_line_right, g_line_right_x - 4, CV_Y);
}

/* UI 线程：用当前竖线/目标值做判定(多盒)，更新 g_judge_label */
void run_judgment(const FramePayload &p) {
  if (!g_judge_label) return;
  if (p.orig_w <= 0 || p.frame.empty()) {
    lv_label_set_text(g_judge_label, "等待画面...");
    lv_obj_set_style_text_color(g_judge_label, lv_color_white(), 0);
    return;
  }
  float llf = g_line_left_x.load() / (float)CV_W;
  float rrf = g_line_right_x.load() / (float)CV_W;
  int target = g_target_count.load();
  auto boxes = judge_all_boxes(p.results, p.orig_w, llf, rrf, target,
                               g_box_class.load(), g_material_class.load());
  JudgeSummary s = summarize(boxes);
  char buf[256];
  if (s.total == 0) {
    lv_label_set_text(g_judge_label, "未检测到盒子");
    lv_obj_set_style_text_color(g_judge_label, lv_color_make(200, 200, 0), 0);
    return;
  }
  snprintf(buf, sizeof(buf),
           "共%d盒：#00FF00 满%d#  #FF3030 未满%d#  #FFA500 未漏出%d#",
           s.total, s.full, s.not_full, s.not_revealed);
  lv_label_set_text(g_judge_label, buf);
  /* 整体判定颜色：全满绿，有未满红，仅未漏出橙 */
  lv_color_t c = (s.not_full > 0)          ? lv_color_make(255, 48, 48)
                 : (s.not_revealed > 0)    ? lv_color_make(255, 165, 0)
                                            : lv_color_make(0, 255, 0);
  lv_obj_set_style_text_color(g_judge_label, c, 0);
}

void on_start(lv_event_t *) {
  if (g_state == ST_RUNNING) return;
  if (g_yolo_path.empty() || g_label_path.empty() || g_input_path.empty()) {
    lv_label_set_text(g_status_label, "请先选择 输入/YOLO模型/标签");
    return;
  }
  /* 采集 UI 参数 */
  g_cfg.input_path = g_input_path;
  g_cfg.yolo_path = g_yolo_path;
  g_cfg.label_path = g_label_path;
  g_cfg.sam_enc_path = g_sam_enc_path;
  g_cfg.sam_dec_path = g_sam_dec_path;
  g_cfg.use_sam = g_use_sam;
  g_cfg.yolo_threads = g_yolo_threads;
  g_cfg.out_dir = "outputs";
  fs::create_directories(g_cfg.out_dir);

  apply_resolution();
  g_bus.reset();
  if (g_worker.joinable()) g_worker.join();
  g_state = ST_RUNNING;
  update_status_text();
  lv_obj_add_state(g_start_btn, LV_STATE_DISABLED);
  lv_obj_clear_state(g_stop_btn, LV_STATE_DISABLED);
  g_worker = std::thread(worker_fn);
}

void on_stop(lv_event_t *) {
  if (g_state != ST_RUNNING) return;
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  g_state = ST_STOPPED;
  update_status_text();
  lv_obj_clear_state(g_start_btn, LV_STATE_DISABLED);
  lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
}

/* 主循环调用：检测 worker 自然结束后回收线程并恢复「开始」按钮。
 * 这解决了"跑完一次后再点开始无反应"的问题（之前按钮一直处于禁用态）。 */
static int g_last_state = ST_IDLE;
void check_worker_done() {
  int s = g_state.load();
  if (g_last_state == ST_RUNNING && (s == ST_DONE || s == ST_STOPPED)) {
    if (g_worker.joinable()) g_worker.join();
    lv_obj_clear_state(g_start_btn, LV_STATE_DISABLED);
    lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
    update_status_text();
  }
  g_last_state = s;
}

void on_quit(lv_event_t *) {
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  camera_capture::shutdown();
  can_bus::shutdown();
  exit(0);
}

void sync_config_to_ui() {
  g_input_path = config::g.default_input;
  g_yolo_path = config::g.yolo_model;
  g_label_path = config::g.label_path;
  g_sam_enc_path = config::g.sam_encoder;
  g_sam_dec_path = config::g.sam_decoder;
  g_use_sam = config::g.use_sam;
  g_yolo_threads = config::g.yolo_threads;
  g_target_count.store(config::g.target_count);
  g_material_class.store(config::g.material_class);
  g_box_class.store(config::g.box_class);
  g_line_left_x.store((int)(config::g.line_left_frac * CV_W));
  g_line_right_x.store((int)(config::g.line_right_frac * CV_W));

  if (g_input_label) lv_label_set_text(g_input_label, base_name(g_input_path).c_str());
  if (g_yolo_label) lv_label_set_text(g_yolo_label, base_name(g_yolo_path).c_str());
  if (g_label_label) lv_label_set_text(g_label_label, base_name(g_label_path).c_str());
  if (g_target_label) {
    char b[16];
    snprintf(b, sizeof(b), "%d", g_target_count.load());
    lv_label_set_text(g_target_label, b);
  }
  if (g_line_left) lv_obj_set_pos(g_line_left, g_line_left_x.load() - 4, CV_Y);
  if (g_line_right) lv_obj_set_pos(g_line_right, g_line_right_x.load() - 4, CV_Y);
  if (g_has_last) run_judgment(g_last_payload);
}

}  // namespace

int run_ui_mode(const AppConfig &cfg) {
  if (ui_init(WIN_W, WIN_H) != 0) {
    SPDLOG_ERROR("ui_init failed");
    return -1;
  }
  SPDLOG_INFO("UI mode started ({}x{})", WIN_W, WIN_H);

  g_input_path = config::g.default_input;
  g_yolo_path = config::g.yolo_model;
  g_label_path = config::g.label_path;
  g_sam_enc_path = config::g.sam_encoder;
  g_sam_dec_path = config::g.sam_decoder;
  g_use_sam = config::g.use_sam;
  g_yolo_threads = config::g.yolo_threads;
  g_target_count.store(config::g.target_count);
  g_material_class.store(config::g.material_class);
  g_box_class.store(config::g.box_class);
  g_line_left_x.store((int)(config::g.line_left_frac * CV_W));
  g_line_right_x.store((int)(config::g.line_right_frac * CV_W));

  /* 命令行输入路径覆盖配置文件 */
  if (!cfg.input_path.empty()) g_input_path = cfg.input_path;

  /* ===== 顶部：仅菜单按钮行(界面标题用 SDL 窗口标题，不在界面内显示) ===== */
  lv_obj_t *topbar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(topbar, WIN_W, TOP_H);
  lv_obj_set_pos(topbar, 0, 0);
  lv_obj_set_style_bg_color(topbar, lv_color_make(28, 36, 56), 0);
  lv_obj_set_style_pad_all(topbar, 4, 0);
  lv_obj_set_style_border_width(topbar, 0, 0);
  lv_obj_clear_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);
  lv_font_t *sfont = ui_font_small();
  /* 按钮容器(靠左，宽度自适应内容) */
  lv_obj_t *btns = lv_obj_create(topbar);
  lv_obj_remove_style_all(btns);
  lv_obj_set_height(btns, TOP_H - 8);
  lv_obj_set_width(btns, LV_SIZE_CONTENT);
  lv_obj_align(btns, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(btns, 5, 0);
  lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
  auto mk_topbtn = [&](const char *txt, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(btns);
    lv_obj_set_height(b, 32);
    lv_obj_set_width(b, LV_SIZE_CONTENT);  /* 自适应文字宽度，防吞字 */
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    if (sfont) lv_obj_set_style_text_font(l, sfont, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
  };
  mk_topbtn("YOLO模型", open_yolo_browser);
  mk_topbtn("标签文件", open_label_browser);
  mk_topbtn("输入源", open_input_source_menu);
  g_start_btn = mk_topbtn("开始", on_start);
  g_stop_btn = mk_topbtn("停止", on_stop);
  lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
  g_quit_btn = mk_topbtn("退出", on_quit);

  /* ===== 左侧：实时推理显示 + 当前物料信息 ===== */
  lv_obj_t *left = lv_obj_create(lv_scr_act());
  lv_obj_set_size(left, LEFT_W, WIN_H - TOP_H);
  lv_obj_set_pos(left, 0, TOP_H);
  lv_obj_set_style_bg_color(left, lv_color_make(18, 18, 18), 0);
  lv_obj_set_style_pad_all(left, 0, 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
  g_canvas = ui_canvas_create(left, CV_W, CV_H);
  lv_obj_set_pos(g_canvas, 0, CV_Y);
  /* 左下：物料信息(每图追加一行) */
  lv_obj_t *info_title = lv_label_create(left);
  lv_label_set_text(info_title, "物料信息");
  lv_obj_set_style_text_color(info_title, lv_color_make(170, 170, 170), 0);
  if (sfont) lv_obj_set_style_text_font(info_title, sfont, 0);
  lv_obj_align(info_title, LV_ALIGN_TOP_LEFT, 6, CV_H + 2);
  g_info_box = lv_textarea_create(left);
  lv_textarea_set_text(g_info_box, "");
  lv_obj_set_size(g_info_box, LEFT_W - 12, WIN_H - TOP_H - CV_H - 30);
  lv_obj_align(g_info_box, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_textarea_set_cursor_click_pos(g_info_box, false);
  if (sfont) lv_obj_set_style_text_font(g_info_box, sfont, 0);

  /* ===== 右侧：生产统计 + 信息输出 ===== */
  lv_obj_t *right = lv_obj_create(lv_scr_act());
  lv_obj_set_size(right, PANEL_W, WIN_H - TOP_H);
  lv_obj_set_pos(right, LEFT_W, TOP_H);
  lv_obj_set_style_pad_all(right, 6, 0);
  lv_obj_set_style_border_width(right, 0, 0);
  lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

  /* --- 生产统计(上) --- */
  constexpr int STATS_H = 300;
  lv_obj_t *stats = lv_obj_create(right);
  lv_obj_set_size(stats, PANEL_W - 12, STATS_H);
  lv_obj_align(stats, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_pad_all(stats, 6, 0);
  lv_obj_set_style_border_width(stats, 1, 0);
  lv_obj_t *st_title = lv_label_create(stats);
  lv_label_set_text(st_title, "生产统计");
  lv_obj_align(st_title, LV_ALIGN_TOP_MID, 0, 0);
  /* 正确/错误 大数字 */
  g_stat_label = lv_label_create(stats);
  lv_label_set_recolor(g_stat_label, true);
  lv_label_set_text(g_stat_label, "#00FF00 正确:0#  #FF3030 错误:0#");
  lv_obj_align(g_stat_label, LV_ALIGN_TOP_LEFT, 6, 26);
  /* 当前帧判定结果 */
  g_judge_label = lv_label_create(stats);
  lv_label_set_recolor(g_judge_label, true);
  lv_label_set_text(g_judge_label, "等待...");
  lv_obj_align(g_judge_label, LV_ALIGN_TOP_LEFT, 6, 58);
  /* 状态行 */
  g_status_label = lv_label_create(stats);
  lv_label_set_text(g_status_label, "状态: 待机");
  lv_obj_align(g_status_label, LV_ALIGN_TOP_LEFT, 6, 86);
  /* 目标物料数 +/- */
  /* 目标物料数 +/- (下移，避开状态行) */
  lv_obj_t *tl2 = lv_label_create(stats);
  lv_label_set_text(tl2, "目标物料数");
  lv_obj_align(tl2, LV_ALIGN_TOP_LEFT, 6, 128);
  if (sfont) lv_obj_set_style_text_font(tl2, sfont, 0);
  g_target_label = lv_label_create(stats);
  {
    char b[16];
    snprintf(b, sizeof(b), "%d", g_target_count.load());
    lv_label_set_text(g_target_label, b);
  }
  lv_obj_align(g_target_label, LV_ALIGN_TOP_LEFT, 150, 128);
  if (sfont) lv_obj_set_style_text_font(g_target_label, sfont, 0);
  auto tgt_btn = [&](const char *txt, int x, bool plus) {
    lv_obj_t *b = lv_btn_create(stats);
    lv_obj_set_size(b, 32, 30);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 124);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(
        b,
        +[](lv_event_t *e) {
          lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
          bool plus2 = (bool)(intptr_t)lv_event_get_user_data(e);
          int v = g_target_count.load();
          if (plus2 && v < 999) g_target_count.store(v + 1);
          if (!plus2 && v > 1) g_target_count.store(v - 1);
          config::g.target_count = g_target_count.load();
          config::mark_dirty();
          char bb[16];
          snprintf(bb, sizeof(bb), "%d", g_target_count.load());
          if (g_target_label) lv_label_set_text(g_target_label, bb);
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)plus);
    return b;
  };
  tgt_btn("-", 200, false);
  tgt_btn("+", 240, true);
  /* SAM 分割：已从界面移除(如需改 use_sam 请编辑 config.json) */
  /* 当前文件信息：模型文件 / 标签文件 / 输入源 (前缀 + 文件名) */
  auto mk_fileinfo = [&](const char *prefix, lv_obj_t *&val_lbl, int y,
                         const std::string &val) {
    lv_obj_t *p = lv_label_create(stats);
    lv_label_set_text(p, prefix);
    lv_obj_align(p, LV_ALIGN_TOP_LEFT, 6, y);
    if (sfont) lv_obj_set_style_text_font(p, sfont, 0);
    val_lbl = lv_label_create(stats);
    lv_label_set_text(val_lbl, val.c_str());
    lv_obj_align(val_lbl, LV_ALIGN_TOP_LEFT, 96, y);
    if (sfont) lv_obj_set_style_text_font(val_lbl, sfont, 0);
    lv_obj_set_width(val_lbl, PANEL_W - 110);
    lv_label_set_long_mode(val_lbl, LV_LABEL_LONG_DOT);
  };
  mk_fileinfo("模型文件:", g_yolo_label, 168, base_name(g_yolo_path));
  mk_fileinfo("标签文件:", g_label_label, 192, base_name(g_label_path));
  mk_fileinfo("输入源:", g_input_label, 216, base_name(g_input_path));

  /* --- 信息输出(下)：单视图 + 两个切换按钮(运行日志 / 警告和报警) --- */
  lv_obj_t *logs = lv_obj_create(right);
  lv_obj_set_size(logs, PANEL_W - 12, WIN_H - TOP_H - STATS_H - 12);
  lv_obj_align(logs, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_all(logs, 4, 0);
  lv_obj_set_style_border_width(logs, 1, 0);
  lv_obj_clear_flag(logs, LV_OBJ_FLAG_SCROLLABLE);
  lv_font_t *sfl = ui_font_small();
  auto logbtn = [&](const char *txt, int x, int mode) {
    lv_obj_t *b = lv_btn_create(logs);
    lv_obj_set_height(b, 28);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, 2);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    if (sfl) lv_obj_set_style_text_font(l, sfl, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(
        b,
        +[](lv_event_t *e) {
          g_log_mode = (int)(intptr_t)lv_event_get_user_data(e);
        },
        LV_EVENT_CLICKED, (void *)(intptr_t)mode);
    return b;
  };
  logbtn("运行日志", 4, 0);
  logbtn("警告和报警", 130, 1);
  /* 单个日志视图(占满剩余高度) */
  g_log_view = lv_textarea_create(logs);
  lv_textarea_set_text(g_log_view, "");
  lv_obj_set_size(g_log_view, lv_pct(100), WIN_H - TOP_H - STATS_H - 12 - 40);
  lv_obj_align(g_log_view, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_textarea_set_cursor_click_pos(g_log_view, false);
  if (sfl) lv_obj_set_style_text_font(g_log_view, sfl, 0);

  /* 启动时自动加载摄像头(若 config 启用) */
  if (config::g.camera_enabled) {
    g_use_camera = true;
    camera_capture::init(0);
    if (g_input_label) lv_label_set_text(g_input_label, "摄像头");
  }

  apply_resolution();

  /* 日志栏滚动缓冲(只保留末尾若干字符) */
  std::string log_info_buf, log_dbgerr_buf;
  int last_log_mode = -1;            /* 检测日志模式切换 */
  std::string info_buf;              /* 物料信息历史(每图追加一行) */
  int info_idx = 0;
  auto append_log = [](std::string &buf, const std::vector<std::string> &lines) {
    for (auto &l : lines) {
      buf.append(l);
      buf.append("\n");
    }
    if (buf.size() > 4000) buf.erase(0, buf.size() - 4000);
  };

  /* 主循环：永不因推理完成退出，只在 退出按钮 时结束。
   * 存最近一帧，每轮用当前竖线/目标值重判 → 拖线时判定实时更新。 */
  while (true) {
    FramePayload payload;
    if (g_bus.pop(payload)) {
      g_last_payload = payload;
      g_has_last = true;
      ui_canvas_set_bgr(payload.frame);
      /* 物料信息：每处理一张图追加一行(图1: ... / 图2: ...) */
      if (g_info_box) {
        info_idx++;
        std::string line = "图" + std::to_string(info_idx) + ": ";
        int bc = g_box_class.load(), mc = g_material_class.load();
        int tgt = g_target_count.load();
        float llf = g_line_left_x.load() / (float)CV_W;
        float rrf = g_line_right_x.load() / (float)CV_W;
        auto boxes = judge_all_boxes(payload.results, payload.orig_w, llf, rrf,
                                     tgt, bc, mc);
        int bi = 1;
        for (auto &b : boxes) {
          const char *st = !b.revealed ? "未漏出"
                           : b.full    ? "满"
                                       : "未满";
          char lb[96];
          snprintf(lb, sizeof(lb), "盒%d(%d/%d)%s ", bi++,
                   b.revealed ? b.material_count : 0, tgt, st);
          line += lb;
        }
        if (boxes.empty()) line += "(无盒子)";
        info_buf.append(line);
        info_buf.append("\n");
        if (info_buf.size() > 4000) info_buf.erase(0, info_buf.size() - 4000);
        lv_textarea_set_text(g_info_box, info_buf.c_str());
        lv_textarea_set_cursor_pos(g_info_box, LV_TEXTAREA_CURSOR_LAST);
      }
    }
    if (g_has_last) run_judgment(g_last_payload);

    /* 摄像头模式空闲时：实时显示画面(无需点"开始") */
    static auto g_last_cam = std::chrono::steady_clock::now();
    if (g_use_camera && g_state != ST_RUNNING && camera_capture::is_ready()) {
      auto now = std::chrono::steady_clock::now();
      if (now - g_last_cam > std::chrono::milliseconds(60)) {
        cv::Mat frame;
        if (camera_capture::grab(frame)) {
          g_last_payload = FramePayload{frame, {}, frame.cols, frame.rows};
          g_has_last = true;
          ui_canvas_set_bgr(frame);
        }
        g_last_cam = now;
      }
    }

    /* 实时刷新累计 正确/错误 计数 */
    if (g_stat_label) {
      char sb[64];
      snprintf(sb, sizeof(sb), "#00FF00 正确: %d#    #FF3030 错误: %d#",
               g_correct_count.load(), g_wrong_count.load());
      lv_label_set_text(g_stat_label, sb);
    }

    /* 日志：双缓冲都追加；仅在 有新内容 或 切换模式 时刷新视图，
     * 其余时间不动 → 允许鼠标上下拖动/滚轮查看历史。 */
    auto ni = ui_log::take_info();
    auto nd = ui_log::take_dbgerr();
    append_log(log_info_buf, ni);
    append_log(log_dbgerr_buf, nd);
    bool mode_changed = (g_log_mode != last_log_mode);
    bool cur_new = (g_log_mode == 0 ? !ni.empty() : !nd.empty());
    if (g_log_view && (mode_changed || cur_new)) {
      const std::string &shown =
          (g_log_mode == 0) ? log_info_buf : log_dbgerr_buf;
      lv_textarea_set_text(g_log_view, shown.c_str());
      lv_textarea_set_cursor_pos(g_log_view, LV_TEXTAREA_CURSOR_LAST);
      last_log_mode = g_log_mode;
    }

    if (g_state != ST_RUNNING && config::poll_hot_reload())
      sync_config_to_ui();
    if (config::poll_save_due())
      config::save();

    ui_tick();
    check_worker_done();
    usleep(5000);
  }
  return 0;
}
