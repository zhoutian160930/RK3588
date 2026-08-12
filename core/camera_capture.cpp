#include "camera_capture.h"
#include "config.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>

#include "SciCam.h"
#include "SciCamPayload.h"

namespace camera_capture {

static void *g_handle = nullptr;
static int g_w = 0, g_h = 0;
static bool g_ready = false;
static std::mutex g_mtx;
static SciCamPixelType g_pixel_type = Mono8;  /* 灰度相机默认 Mono8 */

/* 将值向下对齐到 increment。 */
static int64_t align_down(int64_t v, int64_t inc) {
  if (inc <= 1) return v;
  return v - (v % inc);
}

/* 显式设置相机采集分辨率。
 *   camera_width/height == 0 : 设为传感器原生满分辨率(复位相机可能残留的脏 ROI 配置)
 *   camera_width/height > 0  : 居中裁剪到目标尺寸(ROI)
 * 必须在 StartGrabbing 之前调用。调用方持有 g_mtx。 */
static void apply_roi() {
  SCI_NODE_VAL_INT w{}, h{}, offx{}, offy{};
  unsigned int r1 = SciCam_GetIntValue(g_handle, "Width", &w);
  unsigned int r2 = SciCam_GetIntValue(g_handle, "Height", &h);
  unsigned int r3 = SciCam_GetIntValue(g_handle, "OffsetX", &offx);
  unsigned int r4 = SciCam_GetIntValue(g_handle, "OffsetY", &offy);
  if (r1 || r2 || r3 || r4) {
    SPDLOG_WARN("camera: 分辨率节点读取失败 ({} {} {} {}), 沿用相机当前配置",
                r1, r2, r3, r4);
    return;
  }

  int64_t sen_w = w.nMax;        /* 传感器宽(Width 节点最大值) */
  int64_t sen_h = h.nMax;
  /* 目标尺寸：config>0 用 config，否则用传感器原生满分辨率 */
  int64_t tw = config::g.camera_width > 0 ? config::g.camera_width : sen_w;
  int64_t th = config::g.camera_height > 0 ? config::g.camera_height : sen_h;
  tw = std::max(w.nMin, std::min(align_down(tw, w.nInc), w.nMax));
  th = std::max(h.nMin, std::min(align_down(th, h.nInc), h.nMax));

  /* GenICam 安全顺序：先把 Offset 归零 → 设 Width/Height → 再设居中 Offset。
   * 否则 Width+OffsetX 超过 WidthMax 会被拒。 */
  SciCam_SetIntValue(g_handle, "OffsetX", offx.nMin);
  SciCam_SetIntValue(g_handle, "OffsetY", offy.nMin);
  SciCam_SetIntValue(g_handle, "Width", tw);
  SciCam_SetIntValue(g_handle, "Height", th);
  int64_t cx = std::max(offx.nMin, align_down((sen_w - tw) / 2, offx.nInc));
  int64_t cy = std::max(offy.nMin, align_down((sen_h - th) / 2, offy.nInc));
  SciCam_SetIntValue(g_handle, "OffsetX", cx);
  SciCam_SetIntValue(g_handle, "OffsetY", cy);

  /* 读回实际值 */
  SCI_NODE_VAL_INT wa{}, ha{};
  SciCam_GetIntValue(g_handle, "Width", &wa);
  SciCam_GetIntValue(g_handle, "Height", &ha);
  SPDLOG_INFO("camera: 分辨率设置 {}x{} -> {}x{} (off={},{})",
              (int)w.nVal, (int)h.nVal, (int)wa.nVal, (int)ha.nVal,
              (int)cx, (int)cy);
}

bool init(int timeout_ms) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_ready) return true;
  if (timeout_ms <= 0) timeout_ms = config::g.camera_timeout_ms;

  /* 配置 eth1 IP */
  std::string ip_cmd =
      "ip addr show " + config::g.camera_iface + " | grep -q " +
      config::g.camera_ip.substr(0, config::g.camera_ip.find('/')) +
      " || ip addr add " + config::g.camera_ip + " dev " +
      config::g.camera_iface + " 2>/dev/null";
  system(ip_cmd.c_str());

  /* 发现相机 */
  SPDLOG_INFO("camera: 正在发现相机...");
  SCI_DEVICE_INFO_LIST devs;
  memset(&devs, 0, sizeof(devs));
  unsigned int ret = SciCam_DiscoveryDevices(&devs, 1);
  if (ret != 0 || devs.count == 0) {
    SPDLOG_ERROR("camera: 未发现相机 (ret={}, count={})", ret, devs.count);
    return false;
  }

  SCI_DEVICE_GIGE_INFO &gig = devs.pDevInfo[0].info.gigeInfo;
  char ip[32];
  inet_ntop(AF_INET, &gig.ip, ip, sizeof(ip));
  SPDLOG_INFO("camera: 发现 {}  SN:{}  IP:{}", gig.modelName,
              gig.serialNumber, ip);

  /* 创建设备并打开 */
  ret = SciCam_CreateDevice(&g_handle, &devs.pDevInfo[0]);
  if (ret != 0) {
    SPDLOG_ERROR("camera: CreateDevice 失败 ({})", ret);
    return false;
  }
  ret = SciCam_OpenDevice(g_handle);
  if (ret != 0) {
    SPDLOG_ERROR("camera: OpenDevice 失败 ({})", ret);
    SciCam_DeleteDevice(g_handle);
    g_handle = nullptr;
    return false;
  }

  SciCam_SetEnumValue(g_handle, "ExposureAuto", 2);
  SciCam_SetEnumValue(g_handle, "GainAuto", 2);

  /* 显式设置采集分辨率：默认(0)复位为传感器原生满分辨率，覆盖相机残留的脏配置 */
  apply_roi();

  SciCam_SetGrabTimeout(g_handle, 5000);
  /* Latest 策略：grab() 总返回最新帧并丢弃旧帧，避免慢消费时帧在缓冲区
   * 排队造成 UI 画面滞后。缓冲数降到 2 进一步压低排队深度。 */
  SciCam_SetGrabStrategy(g_handle,
                          SciCamGrabStrategy::SciCam_GrabStrategy_Latest);
  SciCam_SetGrabBufferCount(g_handle, 2);

  ret = SciCam_StartGrabbing(g_handle);
  if (ret != 0) {
    SPDLOG_ERROR("camera: StartGrabbing 失败 ({})", ret);
    SciCam_CloseDevice(g_handle);
    SciCam_DeleteDevice(g_handle);
    g_handle = nullptr;
    return false;
  }

  /* 取第一帧获取分辨率和像素格式 */
  void *payload = nullptr;
  ret = SciCam_Grab(g_handle, &payload);
  if (ret != 0) {
    SPDLOG_ERROR("camera: 首帧抓取失败 ({})", ret);
    SciCam_StopGrabbing(g_handle);
    SciCam_CloseDevice(g_handle);
    SciCam_DeleteDevice(g_handle);
    g_handle = nullptr;
    return false;
  }

  SCI_CAM_PAYLOAD_ATTRIBUTE attr;
  SciCam_Payload_GetAttribute(payload, &attr);
  g_w = (int)attr.imgAttr.width;
  g_h = (int)attr.imgAttr.height;
  g_pixel_type = attr.imgAttr.pixelType;  /* 记录原生像素格式 */
  SciCam_FreePayload(g_handle, payload);

  g_ready = true;
  SPDLOG_INFO("camera: 就绪 {}x{} pixelType=0x{:x} (via SciCamSDK ARM64)",
              g_w, g_h, (unsigned)g_pixel_type);
  return true;
}

bool is_ready() { return g_ready; }

bool grab(cv::Mat &out) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (!g_ready || !g_handle) return false;

  void *payload = nullptr;
  unsigned int ret = SciCam_Grab(g_handle, &payload);
  if (ret != 0) return false;

  SCI_CAM_PAYLOAD_ATTRIBUTE attr;
  SciCam_Payload_GetAttribute(payload, &attr);
  if (!attr.isComplete) {
    SciCam_FreePayload(g_handle, payload);
    return false;
  }

  void *src_img = nullptr;
  SciCam_Payload_GetImage(payload, &src_img);

  /* 根据相机类型选择转换目标：Bayer→BGR8, Mono→Mono8→BGR */
  bool is_bayer = (g_pixel_type == BayerRG8 || g_pixel_type == BayerGR8 ||
                   g_pixel_type == BayerGB8 || g_pixel_type == BayerBG8);

  if (is_bayer) {
    uint64_t dst_size = 0;
    SciCam_Payload_ConvertImage(&attr.imgAttr, src_img, BGR8, nullptr,
                                 &dst_size, true);
    std::vector<uint8_t> buf(dst_size);
    SciCam_Payload_ConvertImage(&attr.imgAttr, src_img, BGR8, buf.data(),
                                 &dst_size, true);
    out = cv::Mat(g_h, g_w, CV_8UC3, buf.data()).clone();
  } else {
    uint64_t dst_size = 0;
    SciCam_Payload_ConvertImage(&attr.imgAttr, src_img, Mono8, nullptr,
                                 &dst_size, true);
    std::vector<uint8_t> buf(dst_size);
    SciCam_Payload_ConvertImage(&attr.imgAttr, src_img, Mono8, buf.data(),
                                 &dst_size, true);
    cv::Mat gray(g_h, g_w, CV_8UC1, buf.data());
    cv::cvtColor(gray.clone(), out, cv::COLOR_GRAY2BGR);
  }

  SciCam_FreePayload(g_handle, payload);
  return true;
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_handle) {
    SciCam_StopGrabbing(g_handle);
    SciCam_CloseDevice(g_handle);
    SciCam_DeleteDevice(g_handle);
    g_handle = nullptr;
  }
  g_ready = false;
  g_w = g_h = 0;
  SPDLOG_INFO("camera: 已关闭");
}

}  // namespace camera_capture
