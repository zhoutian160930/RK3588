//
// Created by kaylor on 3/6/24.
//

#pragma once
#include "image_process.h"
#include "opencv2/opencv.hpp"
#include "queue"
#include "threadpool.h"
#include "yolov8.h"
#include <atomic>

struct YoloResult {
  std::shared_ptr<cv::Mat> img;
  object_detect_result_list results;
  std::string tag;
};

class RknnPool {
 public:
  RknnPool(const std::string model_path, const int thread_num,
           const std::string label_path);
  ~RknnPool();
  void Init();
  void DeInit();
  void AddInferenceTask(std::shared_ptr<cv::Mat> src,
                        ImageProcess image_process,
                        const std::string& tag = "");
  int get_model_id();
  YoloResult GetImageResultFromQueue();
  int GetTasksSize();
  long long GetYoloPreprocessTimeUs() const;
  long long GetYoloInferenceTimeUs() const;
  long long GetYoloProcessedCount() const;

 private:
  int thread_num_{1};
  std::string model_path_{"null"};
  std::string label_path_{"null"};
  uint32_t id{0};
  std::unique_ptr<ThreadPool> pool_;
  std::queue<YoloResult> image_results_;
  std::vector<std::shared_ptr<Yolov8>> models_;
  std::mutex id_mutex_;
  std::mutex image_results_mutex_;
  std::atomic<long long> yolo_preprocess_us_{0};
  std::atomic<long long> yolo_inference_us_{0};
  std::atomic<long long> yolo_task_count_{0};
};
