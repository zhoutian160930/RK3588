#pragma once
#include <opencv2/core.hpp>
#include <cstddef>
#include <string>

namespace image_saver {

/* 启动后台保存线程（幂等）。max_queue: 队列上限，满时丢最旧。 */
void init(std::size_t max_queue = 16);

/* 入队一张图（克隆），由后台线程异步 imwrite 写盘。
 * 队列满时丢弃最旧任务，保证推理线程不被磁盘 IO 阻塞。 */
void enqueue(const cv::Mat &img, const std::string &path);

/* 停止后台线程并等待剩余任务写完。 */
void shutdown();

}  // namespace image_saver
