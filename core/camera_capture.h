#pragma once
#include <opencv2/core.hpp>
#include <string>

namespace camera_capture {

/* 初始化：等待 grab_stream 就绪（/tmp/camera_info.txt），读取分辨率。
 * timeout_ms: 等待超时(ms)，0=无限等。返回 false=超时/失败。 */
bool init(int timeout_ms = 10000);

/* 抓取最新一帧 → out(BGR)。返回 false=无新帧/未初始化。
 * 灰度 Mono8 自动转 BGR 供 YOLO 使用。 */
bool grab(cv::Mat &out);

/* 是否已初始化就绪。 */
bool is_ready();

/* 按 config::g.camera_exposure_us / camera_gain 应用曝光/增益。
 * 0/-1=自动;>0/>=0=手动。热加载时调用,相机就绪即生效,无需重启取流。 */
void apply_exposure();

/* 关闭。 */
void shutdown();
}
