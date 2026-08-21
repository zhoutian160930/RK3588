//
// Created by kaylor on 3/4/24.
//

#pragma once
#include "mutex"
#include "opencv2/opencv.hpp"
#include "postprocess.h"

class ImageProcess {
 public:
  ImageProcess(int width, int height, int target_size);

  /* CPU 路径(原实现):letterbox 到 target×target,保持源像素格式。 */
  std::unique_ptr<cv::Mat> Convert(const cv::Mat &src);

  /* RGA 加速路径(官方 dma-buf 模式):letterbox 到 target×target;
   * to_rgb=true 时同时完成 BGR→RGB(供 NPU 输入,替代 cv::cvtColor)。
   * 实现: 帧拷入 thread_local dma-buf 暂存 → RGA 硬件一次完成
   * 缩放+定位+颜色转换 → 返回持久缓冲的 Mat 头(零拷贝)。
   * ⚠ 返回的 Mat 引用线程私有缓冲,须在该线程下次 Convert 前用完
   *   (现有 rknn_pool 用法满足: 推理后即释放)。
   * 失败策略: dma_heap/import/imcheck/improcess 任一失败自动回退 CPU;
   * 连续失败 3 次进程级粘性降级(永久 CPU,告警一次)。
   * 环境变量 RKNN_FORCE_CPU_PREPROCESS=1 强制 CPU(A/B 对比用)。 */
  std::unique_ptr<cv::Mat> Convert(const cv::Mat &src, bool to_rgb);

  const letterbox_t &get_letter_box();
  void ImagePostProcess(cv::Mat &image, object_detect_result_list &od_results);
  /* 最近一次 Convert(src,to_rgb) 是否走了 RGA(诊断用,每对象独立) */
  bool is_rga_path() const { return rga_active_; }

 private:
  double scale_;
  int padding_x_;
  int padding_y_;
  cv::Size new_size_;
  int target_size_;
  letterbox_t letterbox_;
  bool rga_active_ = false;      /* 最近一帧是否走 RGA(粘性状态在实现内部为进程级原子) */
  void ProcessDetectionImage(cv::Mat &image,
                             object_detect_result_list &od_results) const;
  void ProcessPoseImage(cv::Mat &image,
                        object_detect_result_list &od_results) const;
  void ProcessOBBImage(cv::Mat &image,
                       const object_detect_result_list &od_results) const;
};
