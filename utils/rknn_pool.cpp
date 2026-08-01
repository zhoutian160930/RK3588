//
// Created by kaylor on 3/6/24.
//

#include "rknn_pool.h"

#include <cstdio>
#include "postprocess.h"
#include <chrono>
using Clock = std::chrono::high_resolution_clock;
using Microseconds = std::chrono::microseconds;
RknnPool::RknnPool(const std::string model_path, const int thread_num,
                   const std::string lable_path) {
  this->thread_num_ = thread_num;
  this->model_path_ = model_path;
  this->label_path_ = lable_path;
  this->Init();
}

RknnPool::~RknnPool() {
  if (this->pool_) {
    this->pool_.reset(nullptr);
  }
  this->DeInit();
}

void RknnPool::Init() {
  init_post_process(this->label_path_);
  try {
    // 配置线程池
    this->pool_ = std::make_unique<ThreadPool>(this->thread_num_);
    // 这里每一个线程需要加载一个模型
    for (int i = 0; i < this->thread_num_; ++i) {
      models_.push_back(std::make_shared<Yolov8>(
          std::forward<std::string>(this->model_path_)));
    }
  } catch (const std::bad_alloc &e) {
    printf("[error] Out of memory: %s\n", e.what());
    exit(EXIT_FAILURE);
  }
  for (int i = 0; i < this->thread_num_; ++i) {
    auto ret = models_[i]->Init(models_[0]->get_rknn_context(), i != 0);
    if (ret != 0) {
      printf("[error] Init rknn model failed!\n");
      exit(EXIT_FAILURE);
    }
  }
}



void RknnPool::DeInit() { deinit_post_process(); }

void RknnPool::AddInferenceTask(std::shared_ptr<cv::Mat> src,
                                ImageProcess image_process,
                                const std::string& tag) {
  pool_->enqueue(
      [this, src, image_process, tag](std::shared_ptr<cv::Mat> original_img) mutable {
        auto preprocess_start = Clock::now();
        auto convert_img = image_process.Convert(*original_img);
        auto mode_id = get_model_id();
        cv::Mat rgb_img = cv::Mat::zeros(
            this->models_[mode_id]->get_model_width(),
            this->models_[mode_id]->get_model_height(), convert_img->type());
        cv::cvtColor(*convert_img, rgb_img, cv::COLOR_BGR2RGB);
        auto preprocess_end = Clock::now();
        yolo_preprocess_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(preprocess_end - preprocess_start).count(),
            std::memory_order_relaxed);

        auto infer_start = Clock::now();
        object_detect_result_list od_results;
        this->models_[mode_id]->Inference(rgb_img.ptr(), &od_results,
                                          image_process.get_letter_box());
        auto infer_end = Clock::now();
        yolo_inference_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(infer_end - infer_start).count(),
            std::memory_order_relaxed);

        yolo_task_count_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock_guard(this->image_results_mutex_);
        this->image_results_.push({std::move(original_img), od_results, tag});
      },
      std::move(src));
}

int RknnPool::get_model_id() {
  std::lock_guard<std::mutex> lock(id_mutex_);
  int mode_id = id;
  id++;
  if (id == thread_num_) {
    id = 0;
  }
  //  printf("[info] id = %d, num = %d, mode id = %d\n", id, thread_num_,
  //  mode_id);
  return mode_id;
}

YoloResult RknnPool::GetImageResultFromQueue() {
  std::lock_guard<std::mutex> lock_guard(this->image_results_mutex_);
  if (this->image_results_.empty()) {
    return {nullptr, {}};
  } else {
    auto res = this->image_results_.front();
    this->image_results_.pop();
    return res;
  }
}

int RknnPool::GetTasksSize() { return pool_->TasksSize(); }

long long RknnPool::GetYoloPreprocessTimeUs() const {
  return yolo_preprocess_us_.load(std::memory_order_relaxed);
}

long long RknnPool::GetYoloInferenceTimeUs() const {
  return yolo_inference_us_.load(std::memory_order_relaxed);
}

long long RknnPool::GetYoloProcessedCount() const {
  return yolo_task_count_.load(std::memory_order_relaxed);
}
