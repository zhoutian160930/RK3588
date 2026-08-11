#include "camera_capture.h"
#include "config.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
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
