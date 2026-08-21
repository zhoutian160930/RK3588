//
// Created by kaylor on 3/4/24.
//

#include "image_process.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <string>

/* librga im2d API(头文件位于 utils/mobilesam/,librga.so 已链接) */
#include "im2d.h"
#include "rga.h"

#define N_CLASS_COLORS (20)
unsigned char class_colors[][3] = {
    {255, 56, 56},    // 'FF3838'
    {255, 157, 151},  // 'FF9D97'
    {255, 112, 31},   // 'FF701F'
    {255, 178, 29},   // 'FFB21D'
    {207, 210, 49},   // 'CFD231'
    {72, 249, 10},    // '48F90A'
    {146, 204, 23},   // '92CC17'
    {61, 219, 134},   // '3DDB86'
    {26, 147, 52},    // '1A9334'
    {0, 212, 187},    // '00D4BB'
    {44, 153, 168},   // '2C99A8'
    {0, 194, 255},    // '00C2FF'
    {52, 69, 147},    // '344593'
    {100, 115, 255},  // '6473FF'
    {0, 24, 236},     // '0018EC'
    {132, 56, 255},   // '8438FF'
    {82, 0, 133},     // '520085'
    {203, 56, 255},   // 'CB38FF'
    {255, 149, 200},  // 'FF95C8'
    {255, 55, 199}    // 'FF37C7'
};

ImageProcess::ImageProcess(int width, int height, int target_size) {
  scale_ = static_cast<double>(target_size) / std::max(height, width);
  padding_x_ = target_size - static_cast<int>(width * scale_);
  padding_y_ = target_size - static_cast<int>(height * scale_);
  new_size_ = cv::Size(static_cast<int>(width * scale_),
                       static_cast<int>(height * scale_));
  target_size_ = target_size;
  letterbox_.scale = scale_;
  letterbox_.x_pad = padding_x_ / 2;
  letterbox_.y_pad = padding_y_ / 2;
}

std::unique_ptr<cv::Mat> ImageProcess::Convert(const cv::Mat &src) {
  if (&src == nullptr) {
    return nullptr;
  }
  cv::Mat resize_img;
  cv::resize(src, resize_img, new_size_);
  auto square_img = std::make_unique<cv::Mat>(
      target_size_, target_size_, src.type(), cv::Scalar(114, 114, 114));
  cv::Point position(padding_x_ / 2, padding_y_ / 2);
  resize_img.copyTo((*square_img)(
      cv::Rect(position.x, position.y, resize_img.cols, resize_img.rows)));
  return std::move(square_img);
}

/* ---- RGA 加速路径(官方推荐 dma-buf 模式) ----
 * 参考瑞芯微官方 librga(samples/allocator_demo + FAQ Q1.7/Q1.8):
 *   - 虚拟地址路径每帧做页表转换+强制 cache 同步,实测反而慢于 CPU;
 *   - 官方推荐 dma-buf: 通过 dma_heap 分配, importbuffer_fd 一次导入,
 *     wrapbuffer_handle 每帧封装, blit 走硬件。
 * 流程: memcpy 帧 → uncached dma-buf 暂存 → improcess(缩放+letterbox
 *       定位+BGR→RGB 一次完成) → 返回持久 dst 缓冲的 Mat 头(零拷贝)。
 * 线程模型: RknnPool 按值传递 ImageProcess(每帧拷贝),故缓冲用
 *       thread_local —— 每个推理线程独立一组 dma-buf,分配一次复用。
 * 失败策略: dma_heap/import/imcheck/improcess 任一失败 → 本帧回退 CPU;
 *       连续失败 3 次粘性降级(该线程永久 CPU,只告警一次);
 *       环境变量 RKNN_FORCE_CPU_PREPROCESS=1 强制 CPU(A/B 对比)。
 * 生命周期约束: 返回的 Mat 引用线程私有持久缓冲,调用方须在下次
 *       Convert 前用完(现有 rknn_pool 用法满足: 推理后即释放)。 */

/* 进程级 RGA 状态(ImageProcess 按值传递,每帧拷贝,粘性状态必须进程共享) */
static std::atomic<bool> g_rga_disabled{false};   /* 粘性降级 */
static std::atomic<int> g_rga_fail_count{0};      /* 连续失败计数 */
static std::atomic<bool> g_rga_announced{false};  /* 生效日志只打一次 */

namespace {

/* dma_heap 分配(取自官方 samples/utils/allocator/dma_alloc.cpp) */
struct dma_heap_allocation_data {
  unsigned long long len;
  unsigned int fd;
  unsigned int fd_flags;
  unsigned long long heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC \
  _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)

/* dma-buf cache 同步 ioctl(官方 samples/utils/allocator/dma_alloc.cpp) */
struct dma_buf_sync {
  unsigned long long flags;
};
#define DMA_BUF_BASE 'b'
#define DMA_BUF_IOCTL_SYNC _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_RW    (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START (0 << 2)   /* 设备→CPU 前调用(invalidate) */
#define DMA_BUF_SYNC_END   (1 << 2)   /* CPU→设备前调用(flush) */

int rga_dma_sync(int fd, unsigned long long flags) {
  struct dma_buf_sync sync;
  memset(&sync, 0, sizeof(sync));
  sync.flags = flags;
  return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* src/dst 均用 cached dma32 堆(低 4G 物理地址,避开 RGA IOMMU 32 位限制
 * FAQ Q1.9;CPU 读写走 cache,配合同步 ioctl 保证一致性) */
int rga_dma_alloc(size_t size, int *fd, void **va) {
  /* 首选 system-dma32(cached);不可用时回退 system-uncached(免同步但 CPU 慢) */
  int heap_fd = open("/dev/dma_heap/system-dma32", O_RDWR);
  if (heap_fd < 0) heap_fd = open("/dev/dma_heap/system-uncached", O_RDWR);
  if (heap_fd < 0) return -1;
  dma_heap_allocation_data d;
  memset(&d, 0, sizeof(d));
  d.len = size;
  d.fd_flags = O_CLOEXEC | O_RDWR;
  int ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &d);
  close(heap_fd);
  if (ret < 0) return -1;
  void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, 0);
  if (m == MAP_FAILED) {
    close(d.fd);
    return -1;
  }
  *fd = d.fd;
  *va = m;
  return 0;
}

void rga_dma_free(size_t size, int *fd, void *va) {
  if (va && va != MAP_FAILED) munmap(va, size);
  if (fd && *fd >= 0) close(*fd);
  if (fd) *fd = -1;
}

/* 每线程 RGA 上下文: src 暂存(按需扩容,宽度 16 对齐 stride) + dst(按 target_size) */
struct RgaThreadCtx {
  int src_fd = -1, dst_fd = -1;
  void *src_va = nullptr, *dst_va = nullptr;
  size_t src_cap = 0;
  int src_w = 0, src_h = 0;         /* 当前暂存几何(stride=align16(w)) */
  int dst_size = 0;                 /* 目标边长(分配时缓存) */
  bool dst_filled = false;          /* dst 是否已预填 114 灰 */
  rga_buffer_handle_t src_handle = 0, dst_handle = 0;
  bool ok = false;                  /* 缓冲是否就绪 */
  ~RgaThreadCtx() {
    if (src_handle) releasebuffer_handle(src_handle);
    if (dst_handle) releasebuffer_handle(dst_handle);
    if (src_va) rga_dma_free(src_cap, &src_fd, src_va);
    if (dst_va) {
      size_t sz = (size_t)dst_size * dst_size * 3;
      rga_dma_free(sz, &dst_fd, dst_va);
    }
  }
};

thread_local RgaThreadCtx tls_rga;

static inline int rga_align16(int v) { return (v + 15) & ~15; }

/* 就绪/扩容线程 dma 缓冲。src 几何变化或 dst_size 变化时(重)分配。
 * src 暂存使用 16 对齐 stride(BGR888 对齐要求,官方 FAQ),任意宽度可走 RGA。 */
bool rga_ensure_buffers(int src_w, int src_h, int target_size) {
  RgaThreadCtx &c = tls_rga;
  if (c.ok && c.src_w == src_w && c.src_h == src_h && c.dst_size == target_size)
    return true;

  const int stride = rga_align16(src_w);
  const size_t need = (size_t)stride * src_h * 3;

  /* src 几何变化 → 重分配(按最大几何 2 倍扩容,减少频繁重分配) */
  if (c.src_w != src_w || c.src_h != src_h) {
    if (c.src_va && c.src_cap >= need && c.src_w > 0) {
      /* 缓冲够大,仅几何变化: 复用内存,更新记录 */
      c.src_w = src_w;
      c.src_h = src_h;
    } else {
      if (c.src_handle) {
        releasebuffer_handle(c.src_handle);
        c.src_handle = 0;
      }
      if (c.src_va) rga_dma_free(c.src_cap, &c.src_fd, c.src_va);
      size_t cap = need;   /* 精确分配: cache 同步开销与 buffer 大小成正比 */
      if (rga_dma_alloc(cap, &c.src_fd, &c.src_va) != 0) {
        c.src_va = nullptr;
        c.src_w = c.src_h = 0;
        return false;
      }
      c.src_cap = cap;
      c.src_handle = importbuffer_fd(c.src_fd, cap);
      if (!c.src_handle) return false;
      c.src_w = src_w;
      c.src_h = src_h;
    }
  }

  /* dst 尺寸变化 → 重分配并预填 114 灰(padding 区域,仅需一次) */
  if (c.dst_size != target_size || !c.dst_va) {
    if (c.dst_handle) {
      releasebuffer_handle(c.dst_handle);
      c.dst_handle = 0;
    }
    if (c.dst_va) {
      size_t old = (size_t)c.dst_size * c.dst_size * 3;
      rga_dma_free(old, &c.dst_fd, c.dst_va);
      c.dst_va = nullptr;
    }
    size_t sz = (size_t)target_size * target_size * 3;
    if (rga_dma_alloc(sz, &c.dst_fd, &c.dst_va) != 0) return false;
    memset(c.dst_va, 114, sz);      /* letterbox 灰底,一次成型 */
    rga_dma_sync(c.dst_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW); /* flush */
    c.dst_filled = true;
    c.dst_size = target_size;
    c.dst_handle = importbuffer_fd(c.dst_fd, sz);
    if (!c.dst_handle) return false;
  }
  c.ok = (c.src_handle && c.dst_handle);
  return c.ok;
}

/* 强制 CPU 开关(进程级,首次调用时读取) */
bool rga_force_cpu() {
  static const bool v = [] {
    const char *e = getenv("RKNN_FORCE_CPU_PREPROCESS");
    return e && e[0] == '1';
  }();
  return v;
}

}  // namespace

std::unique_ptr<cv::Mat> ImageProcess::Convert(const cv::Mat &src, bool to_rgb) {
  rga_active_ = false;

  /* 快速否决:强制 CPU / 粘性降级 / 输入非法(仅支持连续 BGR24) */
  if (rga_force_cpu() || g_rga_disabled.load()) return Convert(src);
  if (&src == nullptr || src.empty() || src.type() != CV_8UC3 ||
      !src.isContinuous()) {
    return Convert(src);
  }
  /* 宽度非 16 对齐(BGR888 对齐要求)→ 直接走 CPU。
   * 实测依据: strided 逐行拷贝 + 整 buffer cache 同步的开销,
   * 在 5MP 非对齐图上比纯 CPU resize 慢约 50%;而相机 1280x1024
   * 恰为 16 对齐,整块拷贝 + RGA 硬件缩放收益最大。 */
  if ((src.cols & 15) != 0) return Convert(src);

  const size_t src_sz = src.total() * src.elemSize();
  const int src_stride = rga_align16(src.cols);
  if (!rga_ensure_buffers(src.cols, src.rows, target_size_)) {
    int fails = ++g_rga_fail_count;
    if (fails == 1)
      fprintf(stderr, "[warn] RGA dma-buf 缓冲不可用,本帧走 CPU\n");
    if (fails >= 3 && !g_rga_disabled.exchange(true))
      fprintf(stderr, "[warn] RGA 预处理永久降级 CPU(dma-buf 不可用)\n");
    return Convert(src);
  }

  /* 拷入 cached 对齐暂存(走 CPU cache,写入快)。
   * 宽度恰好 16 对齐时退化为整块拷贝;否则逐行拷贝到对齐 stride。 */
  if (src_stride == src.cols) {
    memcpy(tls_rga.src_va, src.data, src_sz);
  } else {
    uchar *dstp = (uchar *)tls_rga.src_va;
    const uchar *srcp = src.data;
    const size_t row_bytes = (size_t)src.cols * 3;
    for (int y = 0; y < src.rows; y++) {
      memcpy(dstp + (size_t)y * src_stride * 3, srcp + y * src.step,
             row_bytes);
    }
  }
  /* CPU 写完 → flush,交 RGA 硬件读(官方 cpu_to_device 协议) */
  rga_dma_sync(tls_rga.src_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);

  rga_buffer_t rga_src = wrapbuffer_handle(
      tls_rga.src_handle, src.cols, src.rows, RK_FORMAT_BGR_888, src_stride,
      src.rows);
  rga_buffer_t rga_dst =
      wrapbuffer_handle(tls_rga.dst_handle, target_size_, target_size_,
                        to_rgb ? RK_FORMAT_RGB_888 : RK_FORMAT_BGR_888);
  im_rect srect = {0, 0, src.cols, src.rows};
  im_rect drect = {padding_x_ / 2, padding_y_ / 2, new_size_.width,
                   new_size_.height};
  im_rect prect = {};

  /* 官方校验。注意: imcheck 成功返回 IM_STATUS_NOERROR(=2) */
  IM_STATUS st = imcheck(rga_src, rga_dst, srect, drect);
  if (IM_STATUS_NOERROR != st) {
    if (++g_rga_fail_count >= 3 && !g_rga_disabled.exchange(true))
      fprintf(stderr, "[warn] RGA imcheck 不通过(%s),预处理永久走 CPU\n",
              imStrError(st));
    return Convert(src);
  }

  /* 硬件 blit: 缩放 + letterbox 定位 + 颜色转换一次完成。
   * 注意: improcess 成功返回 IM_STATUS_SUCCESS(=1),与 imcheck 不同! */
  st = improcess(rga_src, rga_dst, {}, srect, drect, prect, -1, nullptr,
                 nullptr, IM_SYNC);
  if (IM_STATUS_SUCCESS != st) {
    fprintf(stderr, "[warn] RGA improcess 失败: %s,本帧走 CPU\n",
            imStrError(st));
    ++g_rga_fail_count;
    if (g_rga_fail_count.load() >= 3 && !g_rga_disabled.exchange(true))
      fprintf(stderr, "[warn] RGA 预处理永久降级 CPU\n");
    return Convert(src);
  }

  /* RGA 写完 dst → invalidate,CPU 后续读取(rknn_inputs_set)走 cache
   * (官方 device_to_cpu 协议;若回退到 uncached 堆此调用无害) */
  rga_dma_sync(tls_rga.dst_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);

  g_rga_fail_count.store(0);
  rga_active_ = true;
  bool expect = false;
  if (g_rga_announced.compare_exchange_strong(expect, true)) {
    printf("[info] RGA dma-buf letterbox 预处理生效 (%dx%d -> %dx%d, %s)\n",
           src.cols, src.rows, target_size_, target_size_,
           to_rgb ? "BGR->RGB" : "BGR");
  }
  /* 零拷贝: Mat 头直接引用线程持久缓冲(见上方生命周期约束) */
  return std::make_unique<cv::Mat>(target_size_, target_size_, CV_8UC3,
                                   tls_rga.dst_va);
}

const letterbox_t &ImageProcess::get_letter_box() { return letterbox_; }

void ImageProcess::ImagePostProcess(cv::Mat &image,
                                    object_detect_result_list &od_results) {
  printf("[info] ImagePostProcess is called\n");
  printf("[info] === Debug: od_results details ===\n");
  printf("[info] Count: %d\n", od_results.count);
  printf("[info] Model Type: %d\n", od_results.model_type);
  if (od_results.count > 0) {
    if (od_results.model_type == ModelType::DETECTION ||
        od_results.model_type == ModelType::V10_DETECTION) {
      for (int i = 0; i < od_results.count; i++) {
        auto &res = od_results.results[i];
        printf(
            "[info] Det[%d]: Class: %s, Prob: %.4f, Box: [Left:%d, Top:%d, Right:%d, "
            "Bottom:%d]\n",
            i, coco_cls_to_name(res.cls_id), res.prop, res.box.left,
            res.box.top, res.box.right, res.box.bottom);
      }
    } else if (od_results.model_type == ModelType::OBB) {
      for (int i = 0; i < od_results.count; i++) {
        auto &res = od_results.results_obb[i];
        printf(
            "[info] OBB[%d]: Class: %s, Prob: %.4f, Box: [x:%d, y:%d, w:%d, h:%d, "
            "theta:%f]\n",
            i, coco_cls_to_name(res.cls_id), res.prop, res.box.x, res.box.y,
            res.box.w, res.box.h, res.box.theta);
      }
    } else if (od_results.model_type == ModelType::POSE) {
      for (int i = 0; i < od_results.count; i++) {
        auto &res = od_results.results_pose[i];
        printf("[info] Pose[%d]\n", i);
      }
    }
  }
  printf("[info] ===============================\n");
  if (od_results.count >= 1) {
    int width = image.rows;
    int height = image.cols;
    auto *ori_img = image.ptr();
    int cls_id = od_results.results[0].cls_id;
    uint8_t *seg_mask = od_results.results_seg[0].seg_mask;
    float alpha = 0.5f;  // opacity
    if (seg_mask != nullptr) {
      for (int j = 0; j < height; j++) {
        for (int k = 0; k < width; k++) {
          int pixel_offset = 3 * (j * width + k);
          if (seg_mask[j * width + k] != 0) {
            ori_img[pixel_offset + 0] = (unsigned char)clamp(
                class_colors[seg_mask[j * width + k] % N_CLASS_COLORS][0] *
                        (1 - alpha) +
                    ori_img[pixel_offset + 0] * alpha,
                0, 255);  // r
            ori_img[pixel_offset + 1] = (unsigned char)clamp(
                class_colors[seg_mask[j * width + k] % N_CLASS_COLORS][1] *
                        (1 - alpha) +
                    ori_img[pixel_offset + 1] * alpha,
                0, 255);  // g
            ori_img[pixel_offset + 2] = (unsigned char)clamp(
                class_colors[seg_mask[j * width + k] % N_CLASS_COLORS][2] *
                        (1 - alpha) +
                    ori_img[pixel_offset + 2] * alpha,
                0, 255);  // b
          }
        }
      }
      free(seg_mask);
    }
  }
  printf("[info] model type is %d\n", od_results.model_type);
  if (od_results.model_type == ModelType::DETECTION || od_results.model_type == ModelType::V10_DETECTION) {
    ProcessDetectionImage(image, od_results);
  } else if (od_results.model_type == ModelType::OBB) {
    ProcessOBBImage(image, od_results);
  } else if (od_results.model_type == ModelType::POSE) {
    ProcessPoseImage(image, od_results);
  }
}

void DrawRotatedRect(cv::Mat &image, float x, float y, float w, float h,
                     float theta, const cv::Scalar &color, int thickness) {
  // 定义旋转矩形的中心，尺寸和旋转角度
  cv::Point2f center(x, y);
  cv::Size2f size(w, h);

  // 创建旋转矩形对象
  cv::RotatedRect rotatedRect(center, size, theta);

  // 获取矩形的四个顶点
  cv::Point2f vertices[4];
  rotatedRect.points(vertices);

  // 绘制矩形的四条边
  for (int i = 0; i < 4; i++) {
    cv::line(image, vertices[i], vertices[(i + 1) % 4], color, thickness);
  }
}

void ImageProcess::ProcessOBBImage(
    cv::Mat &image, const object_detect_result_list &od_results) const {
  printf("[info] ImageProcess::ProcessOBBImage is called, result count is %d\n",
         od_results.count);
  for (int i = 0; i < od_results.count; ++i) {
    auto obb_result = od_results.results_obb[i];
    printf("[info] %s @ xywhθ = (%d %d %d %d %f) %f\n",
           coco_cls_to_name(obb_result.cls_id), obb_result.box.x,
           obb_result.box.y, obb_result.box.w, obb_result.box.h,
           obb_result.box.theta * 180.0 / CV_PI, obb_result.prop);
    DrawRotatedRect(image, obb_result.box.x, obb_result.box.y, obb_result.box.w,
                    obb_result.box.h, obb_result.box.theta * 180.0 / CV_PI,
                    cv::Scalar(0, 255, 0), 2);
  }
}

void ImageProcess::ProcessDetectionImage(
    cv::Mat &image, object_detect_result_list &od_results) const {
  for (int i = 0; i < od_results.count; ++i) {
    object_detect_result *detect_result = &(od_results.results[i]);
    //    if (strcmp(coco_cls_to_name(detect_result->cls_id), "person") == 0){
    //    continue;}
    printf("[info] %s @ (%d %d %d %d) %f\n",
           coco_cls_to_name(detect_result->cls_id), detect_result->box.left,
           detect_result->box.top, detect_result->box.right,
           detect_result->box.bottom, detect_result->prop);
    cv::rectangle(
        image, cv::Point(detect_result->box.left, detect_result->box.top),
        cv::Point(detect_result->box.right, detect_result->box.bottom),
        cv::Scalar(0, 0, 255), 2);
    char text[256];
    sprintf(text, "%s %.1f%%", coco_cls_to_name(detect_result->cls_id),
            detect_result->prop * 100);
    cv::putText(image, text,
                cv::Point(detect_result->box.left, detect_result->box.top + 20),
                cv::FONT_HERSHEY_COMPLEX, 1, cv::Scalar(255, 0, 0), 2,
                cv::LINE_8);
  }
}

void drawSkeleton(cv::Mat &img, const std::vector<cv::Point> &points,
                  const std::vector<int> &pairs, const cv::Scalar &color,
                  int thickness) {
  for (size_t i = 0; i < pairs.size(); i += 2) {
    int index1 = pairs[i];
    int index2 = pairs[i + 1];
    if (points[index1].x != -1 && points[index1].y != -1 &&
        points[index2].x != -1 && points[index2].y != -1) {
      cv::line(img, points[index1], points[index2], color, thickness);
    }
  }
}

void ImageProcess::ProcessPoseImage(
    cv::Mat &image, object_detect_result_list &od_results) const {
  for (int i = 0; i < od_results.count; ++i) {
    object_detect_result *detect_result = &(od_results.results[i]);

    printf("[info] (%d %d %d %d) %f\n", detect_result->box.left,
                       detect_result->box.top, detect_result->box.right,
                       detect_result->box.bottom, detect_result->prop);
    cv::rectangle(
        image, cv::Point(detect_result->box.left, detect_result->box.top),
        cv::Point(detect_result->box.right, detect_result->box.bottom),
        cv::Scalar(0, 0, 255), 2);
    std::vector<cv::Point> points(17);
    for (int j = 0; j < 17; ++j) {
      if (od_results.results_pose[i].visibility[j] <= 0.6) {
        points.at(j) = cv::Point(-1, -1);
        continue;
      }
      points.at(j) = (cv::Point(od_results.results_pose[i].kpt[j * 2 + 0],
                                od_results.results_pose[i].kpt[j * 2 + 1]));
      cv::Point p(od_results.results_pose[i].kpt[j * 2 + 0],
                  od_results.results_pose[i].kpt[j * 2 + 1]);
      cv::circle(image, p, 10, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_AA);
    }
    std::vector<int> pairs = {
        0,  1,   // Nose to left eye
        1,  3,   // Left eye to left ear
        0,  2,   // Nose to right eye
        2,  4,   // Right eye to right ear
        0,  5,   // Nose to left shoulder
        5,  7,   // Left shoulder to left elbow
        7,  9,   // Left elbow to left wrist
        0,  6,   // Nose to right shoulder
        6,  8,   // Right shoulder to right elbow
        8,  10,  // Right elbow to right wrist
        5,  6,   // Left shoulder to right shoulder
        11, 12,  // Left hip to right hip
        11, 5,   // Left hip to left shoulder
        12, 6,   // Right hip to right shoulder
        11, 13,  // Left hip to left knee
        12, 14,  // Right hip to right knee
        13, 15,  // Left knee to left ankle
        14, 16   // Right knee to right ankle
    };
    drawSkeleton(image, points, pairs, cv::Scalar(255, 0, 0), 2);
  }
}
