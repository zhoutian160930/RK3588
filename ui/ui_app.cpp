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
#include "mobilesam/mobilesam_pool.h"
#include "rknn_pool.h"
#include "ui_canvas.h"
#include "ui_filebrowser.h"
#include "ui_main.h"

#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

/* 结果保存根目录：每次运行在其下建 时间戳 子文件夹 */
constexpr const char *SAVE_ROOT = "/home/ubuntu/lvgl/output/detImg";
constexpr const char *LOG_DIR = "/home/ubuntu/lvgl/output/log";

namespace {
/* ---- 布局常量 ---- */
constexpr int WIN_W = 1280, WIN_H = 720;
constexpr int PANEL_W = 320;
constexpr int LEFT_W = WIN_W - PANEL_W;
/* canvas 固定显示+缓冲尺寸(16:9，去掉 zoom，方便竖线对齐) */
constexpr int CV_W = LEFT_W;      /* 960 */
constexpr int CV_H = LEFT_W * 9 / 16; /* 540 */
constexpr int CV_Y = 0;           /* canvas 在 left 容器里的 y */

/* ---- 运行期状态 ---- */
AppConfig g_cfg;
std::string g_input_path = "/home/ubuntu/lvgl/img_test";
std::string g_yolo_path = "/home/ubuntu/lvgl/yolomodel/藏药.rknn";
std::string g_label_path = "/home/ubuntu/lvgl/yolomodel/classes.txt";
std::string g_sam_enc_path;
std::string g_sam_dec_path;
bool g_use_sam = false;
int g_yolo_threads = 3;
int g_res_choice = 0; /* 0=720p, 1=1080p */

/* 两条竖线(检测区左右边界)，单位为 canvas 坐标 [0, CV_W]；atomic 供 worker 实时读 */
std::atomic<int> g_line_left_x{CV_W / 4};     /* 默认 240 */
std::atomic<int> g_line_right_x{CV_W * 3 / 4};/* 默认 720 */
lv_obj_t *g_line_left = nullptr;
lv_obj_t *g_line_right = nullptr;
std::atomic<int> g_target_count{10};          /* 目标物料数(UI 与 worker 共享) */

FrameBus g_bus;
std::thread g_worker;
std::atomic<bool> g_stop{false};
enum UiState { ST_IDLE = 0, ST_RUNNING = 1, ST_DONE = 2, ST_STOPPED = 3 };
std::atomic<int> g_state{ST_IDLE};
std::atomic<int> g_proc_count{0};

/* ---- 控件句柄 ---- */
lv_obj_t *g_canvas;
lv_obj_t *g_input_label, *g_yolo_label, *g_label_label;
lv_obj_t *g_samenc_label, *g_samdec_label;
lv_obj_t *g_samenc_row, *g_samdec_row;
lv_obj_t *g_status_label;
lv_obj_t *g_judge_label;   /* 满料判定结果(大字) */
lv_obj_t *g_start_btn, *g_stop_btn;
lv_obj_t *g_res_label;
lv_obj_t *g_quit_btn;
lv_obj_t *g_spin;          /* YOLO 线程 */
lv_obj_t *g_target_spin;   /* 目标物料数 */

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
  lv_label_set_text(g_input_label, base_name(path).c_str());
}
void on_fb_yolo(const char *path, void *) {
  g_yolo_path = trim_path(path);
  lv_label_set_text(g_yolo_label, base_name(path).c_str());
}
void on_fb_label(const char *path, void *) {
  g_label_path = trim_path(path);
  lv_label_set_text(g_label_label, base_name(path).c_str());
}
void on_fb_samenc(const char *path, void *) {
  g_sam_enc_path = trim_path(path);
  lv_label_set_text(g_samenc_label, base_name(path).c_str());
}
void on_fb_samdec(const char *path, void *) {
  g_sam_dec_path = trim_path(path);
  lv_label_set_text(g_samdec_label, base_name(path).c_str());
}

void open_input_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".jpg,.jpeg,.png,.bmp,.mp4,.avi,.mkv", 1,
                      on_fb_input, NULL);
}
void open_yolo_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl/yolomodel", ".rknn", 0, on_fb_yolo, NULL);
}
void open_label_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl/yolomodel", ".txt", 0, on_fb_label, NULL);
}
void open_samenc_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".rknn", 0, on_fb_samenc, NULL);
}
void open_samdec_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".rknn", 0, on_fb_samdec, NULL);
}

/* ---- 满料判定（UI 标签与 worker 画图共用） ---- */
struct Judgment {
  bool has_box = false;
  bool revealed = false;
  int material_count = 0;
  bool full = false;
  int box_left = 0, box_right = 0, box_top = 0, box_bottom = 0; /* 原图坐标 */
};

/* 共享判定函数：按竖线分数判定盒子是否完全漏出，并数盒内物料(cls0)。
 * line_left_frac/right 为竖线占图像宽度的比例(0~1)，与分辨率无关。 */
Judgment judge_results(const object_detect_result_list &res, int orig_w,
                       float line_left_frac, float line_right_frac,
                       int target) {
  Judgment j;
  int best_w = -1;
  for (int i = 0; i < res.count; i++) {
    if (res.results[i].cls_id == 1) {
      int w = res.results[i].box.right - res.results[i].box.left;
      if (w > best_w) {
        best_w = w;
        j.has_box = true;
        j.box_left = res.results[i].box.left;
        j.box_right = res.results[i].box.right;
        j.box_top = res.results[i].box.top;
        j.box_bottom = res.results[i].box.bottom;
      }
    }
  }
  if (!j.has_box) return j;
  int ll = (int)(line_left_frac * orig_w);
  int rr = (int)(line_right_frac * orig_w);
  j.revealed = (j.box_left >= ll) && (j.box_right <= rr);
  if (!j.revealed) return j;
  for (int i = 0; i < res.count; i++) {
    if (res.results[i].cls_id == 0) {
      int cx = (res.results[i].box.left + res.results[i].box.right) / 2;
      int cy = (res.results[i].box.top + res.results[i].box.bottom) / 2;
      if (cx >= j.box_left && cx <= j.box_right && cy >= j.box_top &&
          cy <= j.box_bottom)
        j.material_count++;
    }
  }
  j.full = (j.material_count >= target);
  return j;
}

/* ---- 推理工作线程 ---- */
void worker_fn() {
  g_state = ST_RUNNING;
  g_proc_count = 0;
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
  RknnPool yolo(cfg.yolo_path, cfg.yolo_threads, cfg.label_path);
  SPDLOG_INFO("YOLO model loaded");
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

  /* 本次运行的结果保存目录：SAVE_ROOT/<时间戳>/ */
  std::time_t now_t = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now_t));
  std::string save_dir = std::string(SAVE_ROOT) + "/" + ts;
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

    /* 画两条竖线 + 判定文字到图上(保存与显示都用) */
    float llf = g_line_left_x.load() / (float)CV_W;
    float rrf = g_line_right_x.load() / (float)CV_W;
    int target = g_target_count.load();
    Judgment jd = judge_results(r.results, out.cols, llf, rrf, target);
    int ll = (int)(llf * out.cols), rr = (int)(rrf * out.cols);
    cv::line(out, cv::Point(ll, 0), cv::Point(ll, out.rows),
             cv::Scalar(255, 255, 0), 2);  /* 左线 青 */
    cv::line(out, cv::Point(rr, 0), cv::Point(rr, out.rows),
             cv::Scalar(255, 0, 255), 2);  /* 右线 品红 */
    /* 判定文字(英文，OpenCV putText 不支持中文) */
    std::string rev_txt = jd.has_box ? (jd.revealed ? "Revealed: YES" : "Revealed: NO") : "No Box";
    cv::Scalar rev_col = jd.revealed ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 128, 255);
    cv::putText(out, rev_txt, cv::Point(15, 35), cv::FONT_HERSHEY_SIMPLEX, 1.0, rev_col, 2);
    char fbuf[64];
    const char *tag = jd.full ? "FULL" : "NOT FULL";
    snprintf(fbuf, sizeof(fbuf), "Material: %s (%d/%d)", tag, jd.material_count, target);
    cv::Scalar fcol = jd.full ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
    cv::putText(out, fbuf, cv::Point(15, 75), cv::FONT_HERSHEY_SIMPLEX, 1.0, fcol, 2);

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

    /* 每图评判结果日志 */
    if (jd.has_box) {
      SPDLOG_INFO(
          "frame {} | box(img)={}~{} lines(frac)={:.2f}~{:.2f} revealed={} "
          "material={}/{} full={} | save={}",
          cur_idx, jd.box_left, jd.box_right, llf, rrf, jd.revealed,
          jd.material_count, target, jd.full, savepath);
    } else {
      SPDLOG_INFO(
          "frame {} | no box(cls1) detected, revealed=false | save={}", cur_idx,
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

  if (is_image) {
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
  if (obj == g_line_left) g_line_left_x = x;
  else if (obj == g_line_right) g_line_right_x = x;
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

/* UI 线程：用当前竖线/目标值做判定，更新 g_judge_label */
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
  Judgment j = judge_results(p.results, p.orig_w, llf, rrf, target);
  char buf[128];
  if (!j.has_box) {
    lv_label_set_text(g_judge_label, "未检测到盒子");
    lv_obj_set_style_text_color(g_judge_label, lv_color_make(200, 200, 0), 0);
    return;
  }
  int bl = (int)(j.box_left * CV_W / (float)p.orig_w);
  int br = (int)(j.box_right * CV_W / (float)p.orig_w);
  if (!j.revealed) {
    snprintf(buf, sizeof(buf), "盒子未完全漏出\n(box %d~%d 不在 %d~%d 内)", bl, br,
             g_line_left_x.load(), g_line_right_x.load());
    lv_label_set_text(g_judge_label, buf);
    lv_obj_set_style_text_color(g_judge_label, lv_color_make(255, 160, 0), 0);
    return;
  }
  if (j.full) {
    snprintf(buf, sizeof(buf), "#00FF00 满物料#  %d/%d", j.material_count, target);
  } else {
    snprintf(buf, sizeof(buf), "#FF3030 未满物料#  %d/%d", j.material_count, target);
  }
  lv_label_set_text(g_judge_label, buf);
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
  g_cfg.yolo_threads = lv_spinbox_get_value(g_spin);
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

void on_res_toggle(lv_event_t *) {
  g_res_choice ^= 1;
  lv_label_set_text(g_res_label, g_res_choice ? "1080p" : "720p");
  if (g_state != ST_RUNNING) apply_resolution();
}

void on_sam_toggle(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_use_sam = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (g_use_sam) {
    lv_obj_clear_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);
  }
}

void on_quit(lv_event_t *) {
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  exit(0);
}

/* 创建一行：标题 + 值标签 + 选择按钮，返回行容器。 */
lv_obj_t *add_row(lv_obj_t *parent, const char *title, lv_obj_t **value_label,
                  lv_event_cb_t pick_cb, const char *pick_txt) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, PANEL_W - 20, 56);
  lv_obj_set_style_pad_all(row, 2, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = lv_label_create(row);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *v = lv_label_create(row);
  lv_label_set_text(v, "(未选)");
  lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_width(v, PANEL_W - 90);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  if (value_label) *value_label = v;
  if (pick_cb) {
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_size(b, 56, 30);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, pick_txt);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, NULL);
  }
  return row;
}

}  // namespace

int run_ui_mode(const AppConfig &cfg) {
  if (ui_init(WIN_W, WIN_H) != 0) {
    SPDLOG_ERROR("ui_init failed");
    return -1;
  }
  SPDLOG_INFO("UI mode started ({}x{})", WIN_W, WIN_H);

  /* 用命令行传入的输入路径作为默认（模型/标签沿用本机可用默认值） */
  if (!cfg.input_path.empty()) g_input_path = cfg.input_path;

  /* 左侧画布区 */
  lv_obj_t *left = lv_obj_create(lv_scr_act());
  lv_obj_set_size(left, LEFT_W, WIN_H);
  lv_obj_set_style_bg_color(left, lv_color_make(20, 20, 20), 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_set_style_pad_all(left, 0, 0);
  lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
  g_canvas = ui_canvas_create(left, CV_W, CV_H);
  lv_obj_set_pos(g_canvas, 0, CV_Y);

  /* 判定结果大标签(canvas 下方) */
  g_judge_label = lv_label_create(left);
  lv_label_set_text(g_judge_label, "等待开始...");
  lv_label_set_recolor(g_judge_label, true);
  lv_obj_set_style_text_color(g_judge_label, lv_color_white(), 0);
  lv_obj_align(g_judge_label, LV_ALIGN_TOP_LEFT, 8, CV_H + 16);

  /* 右侧控制面板 */
  lv_obj_t *panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(panel, PANEL_W, WIN_H);
  lv_obj_set_pos(panel, LEFT_W, 0);
  lv_obj_set_style_pad_all(panel, 8, 0);
  lv_obj_t *pt = lv_label_create(panel);
  lv_label_set_text(pt, "YOLO 推理控制台");
  lv_obj_align(pt, LV_ALIGN_TOP_MID, 0, 0);

  /* 预填当前值标签 */
  lv_obj_t *r1 = add_row(panel, "输入源(图/视频/目录)", &g_input_label,
                         open_input_browser, "选");
  lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 24);
  lv_label_set_text(g_input_label, base_name(g_input_path).c_str());

  lv_obj_t *r2 = add_row(panel, "YOLO 模型(.rknn)", &g_yolo_label,
                         open_yolo_browser, "选");
  lv_obj_align_to(r2, r1, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(g_yolo_label, base_name(g_yolo_path).c_str());

  lv_obj_t *r3 = add_row(panel, "标签文件(.txt)", &g_label_label,
                         open_label_browser, "选");
  lv_obj_align_to(r3, r2, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(g_label_label, base_name(g_label_path).c_str());

  /* SAM 开关 */
  lv_obj_t *sam_row = lv_obj_create(panel);
  lv_obj_set_size(sam_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(sam_row, 2, 0);
  lv_obj_set_style_border_width(sam_row, 0, 0);
  lv_obj_clear_flag(sam_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(sam_row, r3, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *sl = lv_label_create(sam_row);
  lv_label_set_text(sl, "SAM 分割(掩膜存文件)");
  lv_obj_align(sl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *sw = lv_switch_create(sam_row);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_state(sw, g_use_sam ? LV_STATE_CHECKED : 0);
  lv_obj_add_event_cb(sw, on_sam_toggle, LV_EVENT_VALUE_CHANGED, NULL);

  g_samenc_row = add_row(panel, "SAM encoder(.rknn)", &g_samenc_label,
                         open_samenc_browser, "选");
  lv_obj_align_to(g_samenc_row, sam_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  g_samdec_row = add_row(panel, "SAM decoder(.rknn)", &g_samdec_label,
                         open_samdec_browser, "选");
  lv_obj_align_to(g_samdec_row, g_samenc_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_add_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);

  /* 线程 spinbox */
  lv_obj_t *thr_row = lv_obj_create(panel);
  lv_obj_set_size(thr_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(thr_row, 2, 0);
  lv_obj_set_style_border_width(thr_row, 0, 0);
  lv_obj_clear_flag(thr_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(thr_row, g_samdec_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *tl = lv_label_create(thr_row);
  lv_label_set_text(tl, "YOLO 线程");
  lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *spin = lv_spinbox_create(thr_row);
  lv_spinbox_set_range(spin, 1, 6);
  lv_spinbox_set_digit_format(spin, 1, 0);
  lv_spinbox_set_value(spin, g_yolo_threads);
  lv_obj_align(spin, LV_ALIGN_RIGHT_MID, 0, 0);
  g_spin = spin;

  /* 目标物料数 spinbox（满料判定阈值） */
  lv_obj_t *res_row = lv_obj_create(panel);
  lv_obj_set_size(res_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(res_row, 2, 0);
  lv_obj_set_style_border_width(res_row, 0, 0);
  lv_obj_clear_flag(res_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(res_row, thr_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *tl2 = lv_label_create(res_row);
  lv_label_set_text(tl2, "目标物料数");
  lv_obj_align(tl2, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *tspin = lv_spinbox_create(res_row);
  lv_spinbox_set_range(tspin, 1, 999);
  lv_spinbox_set_digit_format(tspin, 2, 0);
  lv_spinbox_set_value(tspin, g_target_count.load());
  lv_obj_align(tspin, LV_ALIGN_RIGHT_MID, 0, 0);
  g_target_spin = tspin;
  lv_obj_add_event_cb(
      tspin,
      [](lv_event_t *e) {
        lv_obj_t *sp = (lv_obj_t *)lv_event_get_target(e);
        g_target_count.store(lv_spinbox_get_value(sp));
      },
      LV_EVENT_VALUE_CHANGED, NULL);

  /* 开始 / 停止 */
  lv_obj_t *btn_row = lv_obj_create(panel);
  lv_obj_set_size(btn_row, PANEL_W - 20, 48);
  lv_obj_set_style_pad_all(btn_row, 2, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(btn_row, res_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
  g_start_btn = lv_btn_create(btn_row);
  lv_obj_set_size(g_start_btn, 110, 40);
  lv_obj_align(g_start_btn, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *slb = lv_label_create(g_start_btn);
  lv_label_set_text(slb, "开始");
  lv_obj_center(slb);
  lv_obj_add_event_cb(g_start_btn, on_start, LV_EVENT_CLICKED, NULL);
  g_stop_btn = lv_btn_create(btn_row);
  lv_obj_set_size(g_stop_btn, 110, 40);
  lv_obj_align(g_stop_btn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_t *spb = lv_label_create(g_stop_btn);
  lv_label_set_text(spb, "停止");
  lv_obj_center(spb);
  lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
  lv_obj_add_event_cb(g_stop_btn, on_stop, LV_EVENT_CLICKED, NULL);

  /* 状态 */
  g_status_label = lv_label_create(panel);
  lv_obj_align_to(g_status_label, btn_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
  update_status_text();

  /* 退出 */
  g_quit_btn = lv_btn_create(panel);
  lv_obj_set_size(g_quit_btn, PANEL_W - 20, 36);
  lv_obj_align(g_quit_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_t *ql = lv_label_create(g_quit_btn);
  lv_label_set_text(ql, "退出");
  lv_obj_center(ql);
  lv_obj_add_event_cb(g_quit_btn, on_quit, LV_EVENT_CLICKED, NULL);

  apply_resolution();

  /* 主循环：永不因推理完成退出，只在 退出按钮 时结束。
   * 顺序：先 pop+显示+判定，再 ui_tick 渲染。 */
  while (true) {
    FramePayload payload;
    if (g_bus.pop(payload)) {
      ui_canvas_set_bgr(payload.frame);
      run_judgment(payload);
    }
    ui_tick();
    check_worker_done();
    usleep(5000);
  }
  return 0;
}
