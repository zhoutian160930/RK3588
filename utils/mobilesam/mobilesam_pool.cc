#include "mobilesam_pool.h"
#include "rknn_mobilesam_utils.h" // 包含底层 utils 以便拆分 encoder/decoder
#include "image_utils.h"
#include "image_drawing.h"
#include <iostream>
#include <chrono>
using Clock = std::chrono::high_resolution_clock;
using Microseconds = std::chrono::microseconds;

MobileSamPool::MobileSamPool(const std::string encoder_path, const std::string decoder_path, int thread_num)
    : encoder_path_(encoder_path), decoder_path_(decoder_path), thread_num_(thread_num) {}

MobileSamPool::~MobileSamPool() {
    // Ensure all tasks are finished and threads joined before releasing RKNN contexts
    if (pool_) {
        pool_.reset(nullptr);
    }
    for (auto& ctx : contexts_) {
        release_mobilesam_model(ctx.get());
    }
}

int MobileSamPool::Init() {
    try {
        pool_ = std::make_unique<ThreadPool>(thread_num_);
        
        for (int i = 0; i < thread_num_; ++i) {
            auto ctx = std::make_shared<mobilesam_app_context_t>();
            // 初始化每个线程独立的 RKNN Context
            int ret = init_mobilesam_model(encoder_path_.c_str(), decoder_path_.c_str(), ctx.get());
            if (ret != 0) {
                printf("Init model %d failed!\n", i);
                return -1;
            }
            contexts_.push_back(ctx);
        }
    } catch (...) {
        return -1;
    }
    return 0;
}

int MobileSamPool::get_model_id() {
    std::lock_guard<std::mutex> lock(id_mutex_);
    int id = current_model_id_;
    current_model_id_ = (current_model_id_ + 1) % thread_num_;
    return id;
}

int MobileSamPool::GetProcessedCount() {
    std::lock_guard<std::mutex> lock(count_mutex_);
    return processed_count_;
}

void MobileSamPool::AddInferenceTask(const std::string& img_path, 
    const std::string& out_path, const std::vector<mobilesam_box>& boxes) {
    // 将任务 lambda 提交给线程池
    pool_->enqueue([this, img_path, out_path, boxes]() {
        // 1. 获取当前线程绑定的模型 ID
        int model_id = this->get_model_id();
        auto ctx = contexts_[model_id].get();

        // 2. 读取图片
        auto preprocess_start = Clock::now();
        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        if (read_image(img_path.c_str(), &src_image) != 0) {
            printf("Read image failed: %s\n", img_path.c_str());
            return;
        }

        // ==========================================
        // Stage 1: Encoder 推理 (每张图只做一次!)
        // ==========================================
        int img_embeds_size = ctx->encoder.output_attrs[0].n_elems;
        std::vector<float> img_embeds_nchw(img_embeds_size);
        std::vector<float> img_embeds_nhwc(img_embeds_size);
        auto preprocess_end = Clock::now();
        sam_preprocess_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(preprocess_end - preprocess_start).count(),
            std::memory_order_relaxed);

        auto encoder_start = Clock::now();
        int ret = inference_mobilesam_encoder_utils(&(ctx->encoder), &src_image, img_embeds_nchw.data());
        auto encoder_end = Clock::now();
        sam_inference_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(encoder_end - encoder_start).count(),
            std::memory_order_relaxed);
        if (ret != 0) {
            printf("Encoder failed for %s\n", img_path.c_str());
            free(src_image.virt_addr);
            return;
        }

        // NCHW -> NHWC 转换 (Decoder 需要 NHWC)
        rknn_nchw_2_nhwc(img_embeds_nchw.data(), img_embeds_nhwc.data(),
                         ctx->encoder.output_attrs[0].dims[0],
                         ctx->encoder.output_attrs[0].dims[1],
                         ctx->encoder.output_attrs[0].dims[2],
                         ctx->encoder.output_attrs[0].dims[3]);

        // ==========================================
        // Stage 2: Decoder 推理 (循环处理所有 Box)
        // ==========================================
        
        // 准备 Decoder 输出 buffer
        int iou_size = ctx->decoder.output_attrs[0].n_elems;
        int mask_size = ctx->decoder.output_attrs[1].n_elems;
        std::vector<float> iou_predictions(iou_size);
        std::vector<float> low_res_masks(mask_size);
        
        std::vector<float> input_points(4); // 2 points * 2 coords
        std::vector<float> input_labels = {2.0f, 3.0f}; // 2: TopLeft, 3: BottomRight

        for (const auto& box : boxes) {
            auto coords_start = Clock::now();
            float raw_coords[4] = {(float)box.x1, (float)box.y1, (float)box.x2, (float)box.y2};
            point_coords_preprocess(raw_coords, 4, src_image.height, src_image.width, input_points.data());
            auto coords_end = Clock::now();
            sam_preprocess_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(coords_end - coords_start).count(),
                std::memory_order_relaxed);

            auto decoder_start = Clock::now();
            inference_mobilesam_decoder_utils(&(ctx->decoder), 
                                              img_embeds_nhwc.data(), 
                                              input_points.data(), 
                                              input_labels.data(), 
                                              iou_predictions.data(), 
                                              low_res_masks.data());
            auto decoder_end = Clock::now();
            sam_inference_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(decoder_end - decoder_start).count(),
                std::memory_order_relaxed);

            auto post_start = Clock::now();
            mobilesam_res res;
            post_process(ctx, iou_predictions.data(), low_res_masks.data(), &res, src_image.height, src_image.width);

            if (res.mask) {
                draw_mask(&src_image, res.mask);
                free(res.mask); // 记得释放 post_process 分配的内存
            }
            draw_rectangle(&src_image, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1, COLOR_GREEN, 2);
            auto post_end = Clock::now();
            sam_postprocess_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(post_end - post_start).count(),
                std::memory_order_relaxed);
        }

        auto save_start = Clock::now();
        write_image(out_path.c_str(), &src_image);
        auto save_end = Clock::now();
        sam_postprocess_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(save_end - save_start).count(),
            std::memory_order_relaxed);
        
        // 清理
        if (src_image.virt_addr) free(src_image.virt_addr);
        
        // 计数
        {
            std::lock_guard<std::mutex> lock(count_mutex_);
            processed_count_++;
        }
    });
}

void MobileSamPool::AddInferenceTask(const cv::Mat& img_in, const std::string& out_path, const std::vector<mobilesam_box>& boxes) {
    // Clone to ensure data validity in async lambda
    cv::Mat img = img_in.clone();
    pool_->enqueue([this, img, out_path, boxes]() mutable {
        int model_id = this->get_model_id();
        auto ctx = contexts_[model_id].get();

        auto preprocess_start = Clock::now();
        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        src_image.width = img.cols;
        src_image.height = img.rows;
        src_image.width_stride = img.step; 
        src_image.height_stride = img.rows;
        src_image.format = IMAGE_FORMAT_RGB888; // Assuming input is RGB
        src_image.virt_addr = img.data;
        src_image.size = img.total() * img.elemSize();

        // Stage 1: Encoder
        int img_embeds_size = ctx->encoder.output_attrs[0].n_elems;
        std::vector<float> img_embeds_nchw(img_embeds_size);
        std::vector<float> img_embeds_nhwc(img_embeds_size);
        auto preprocess_end = Clock::now();
        sam_preprocess_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(preprocess_end - preprocess_start).count(),
            std::memory_order_relaxed);

        auto encoder_start = Clock::now();
        int ret = inference_mobilesam_encoder_utils(&(ctx->encoder), &src_image, img_embeds_nchw.data());
        auto encoder_end = Clock::now();
        sam_inference_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(encoder_end - encoder_start).count(),
            std::memory_order_relaxed);
        if (ret != 0) {
            printf("Encoder failed for cv::Mat input\n");
            return;
        }

        rknn_nchw_2_nhwc(img_embeds_nchw.data(), img_embeds_nhwc.data(),
                         ctx->encoder.output_attrs[0].dims[0],
                         ctx->encoder.output_attrs[0].dims[1],
                         ctx->encoder.output_attrs[0].dims[2],
                         ctx->encoder.output_attrs[0].dims[3]);

        // Stage 2: Decoder
        int iou_size = ctx->decoder.output_attrs[0].n_elems;
        int mask_size = ctx->decoder.output_attrs[1].n_elems;
        std::vector<float> iou_predictions(iou_size);
        std::vector<float> low_res_masks(mask_size);
        
        std::vector<float> input_points(4); 
        std::vector<float> input_labels = {2.0f, 3.0f};

        for (const auto& box : boxes) {
            auto coords_start = Clock::now();
            float raw_coords[4] = {(float)box.x1, (float)box.y1, (float)box.x2, (float)box.y2};
            point_coords_preprocess(raw_coords, 4, src_image.height, src_image.width, input_points.data());
            auto coords_end = Clock::now();
            sam_preprocess_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(coords_end - coords_start).count(),
                std::memory_order_relaxed);

            auto decoder_start = Clock::now();
            inference_mobilesam_decoder_utils(&(ctx->decoder), 
                                              img_embeds_nhwc.data(), 
                                              input_points.data(), 
                                              input_labels.data(), 
                                              iou_predictions.data(), 
                                              low_res_masks.data());
            auto decoder_end = Clock::now();
            sam_inference_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(decoder_end - decoder_start).count(),
                std::memory_order_relaxed);

            auto post_start = Clock::now();
            mobilesam_res res;
            post_process(ctx, iou_predictions.data(), low_res_masks.data(), &res, src_image.height, src_image.width);

            if (res.mask) {
                draw_mask(&src_image, res.mask);
                free(res.mask);
            }
            draw_rectangle(&src_image, box.x1, box.y1, box.x2 - box.x1, box.y2 - box.y1, COLOR_GREEN, 2);
            auto post_end = Clock::now();
            sam_postprocess_us_.fetch_add(
                std::chrono::duration_cast<Microseconds>(post_end - post_start).count(),
                std::memory_order_relaxed);
        }

        auto save_start = Clock::now();
        write_image(out_path.c_str(), &src_image);
        auto save_end = Clock::now();
        sam_postprocess_us_.fetch_add(
            std::chrono::duration_cast<Microseconds>(save_end - save_start).count(),
            std::memory_order_relaxed);
        
        // Do NOT free src_image.virt_addr as it belongs to cv::Mat

        {
            std::lock_guard<std::mutex> lock(count_mutex_);
            processed_count_++;
        }
    });
}

long long MobileSamPool::GetSamPreprocessTimeUs() const {
    return sam_preprocess_us_.load(std::memory_order_relaxed);
}

long long MobileSamPool::GetSamInferenceTimeUs() const {
    return sam_inference_us_.load(std::memory_order_relaxed);
}

long long MobileSamPool::GetSamPostprocessTimeUs() const {
    return sam_postprocess_us_.load(std::memory_order_relaxed);
}
