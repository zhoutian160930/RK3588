#ifndef MOBILESAM_POOL_H
#define MOBILESAM_POOL_H

#include "threadpool.h" // 你提供的 ThreadPool 头文件
#include "mobilesam.h"   // MobileSAM 原始头文件
#include <vector>
#include <queue>
#include <mutex>
#include <memory>
#include <opencv2/opencv.hpp>
#include <atomic>

// 定义一个任务结构，包含图片路径/数据和该图片对应的所有 Box
struct SamTask {
    std::string img_path;
    std::string out_path;
    std::vector<mobilesam_box> boxes; // 使用 postprocess.h 中定义的 mobilesam_box
};

class MobileSamPool {
public:
    MobileSamPool(const std::string encoder_path, const std::string decoder_path, int thread_num);
    ~MobileSamPool();

    // 初始化模型池
    int Init();
    
    // 添加推理任务：一张图片 + 多个 Prompt Boxes
    void AddInferenceTask(const std::string& img_path, const std::string& out_path, const std::vector<mobilesam_box>& boxes);
    void AddInferenceTask(const cv::Mat& img, const std::string& out_path, const std::vector<mobilesam_box>& boxes);

    // 获取处理完成的结果数（用于主线程判断进度）
    int GetProcessedCount();
    long long GetSamPreprocessTimeUs() const;
    long long GetSamInferenceTimeUs() const;
    long long GetSamPostprocessTimeUs() const;

private:
    // 获取当前线程应使用的模型 ID
    int get_model_id();

private:
    int thread_num_;
    std::string encoder_path_;
    std::string decoder_path_;

    // 模型上下文池，每个线程对应一个 context
    std::vector<std::shared_ptr<mobilesam_app_context_t>> contexts_;
    
    // 线程池
    std::unique_ptr<ThreadPool> pool_;

    // 辅助变量
    std::mutex id_mutex_;
    int current_model_id_ = 0;
    
    std::mutex count_mutex_;
    int processed_count_ = 0;
    std::atomic<long long> sam_preprocess_us_{0};
    std::atomic<long long> sam_inference_us_{0};
    std::atomic<long long> sam_postprocess_us_{0};
};

#endif // MOBILESAM_POOL_H
