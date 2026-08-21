#include "camera_capture.h"
#include "config.h"

#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "MvCameraControl.h"

namespace camera_capture {

static void *g_handle = nullptr;
static int g_w = 0, g_h = 0;
static bool g_ready = false;
static std::mutex g_mtx;
static MvGvspPixelType g_pixel_type = PixelType_Gvsp_Mono8;  /* 灰度相机默认 Mono8 */
static bool g_pixel_warned = false;

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
  MVCC_INTVALUE_EX w{}, h{}, offx{}, offy{};
  int r1 = MV_CC_GetIntValueEx(g_handle, "Width", &w);
  int r2 = MV_CC_GetIntValueEx(g_handle, "Height", &h);
  int r3 = MV_CC_GetIntValueEx(g_handle, "OffsetX", &offx);
  int r4 = MV_CC_GetIntValueEx(g_handle, "OffsetY", &offy);
  if (r1 || r2 || r3 || r4) {
    SPDLOG_WARN("camera: 分辨率节点读取失败 ({} {} {} {}), 沿用相机当前配置",
                r1, r2, r3, r4);
    return;
  }

  int64_t sen_w = (int64_t)w.nMax;        /* 传感器宽(Width 节点最大值) */
  int64_t sen_h = (int64_t)h.nMax;
  /* 目标尺寸：config>0 用 config，否则用传感器原生满分辨率 */
  int64_t tw = config::g.camera_width > 0 ? config::g.camera_width : sen_w;
  int64_t th = config::g.camera_height > 0 ? config::g.camera_height : sen_h;
  tw = std::max<int64_t>(w.nMin, std::min(align_down(tw, w.nInc), (int64_t)w.nMax));
  th = std::max<int64_t>(h.nMin, std::min(align_down(th, h.nInc), (int64_t)h.nMax));

  /* GenICam 安全顺序：先把 Offset 归零 → 设 Width/Height → 再设居中 Offset。
   * 否则 Width+OffsetX 超过 WidthMax 会被拒。 */
  MV_CC_SetIntValueEx(g_handle, "OffsetX", offx.nMin);
  MV_CC_SetIntValueEx(g_handle, "OffsetY", offy.nMin);
  MV_CC_SetIntValueEx(g_handle, "Width", tw);
  MV_CC_SetIntValueEx(g_handle, "Height", th);
  int64_t cx = std::max<int64_t>(offx.nMin, align_down((sen_w - tw) / 2, offx.nInc));
  int64_t cy = std::max<int64_t>(offy.nMin, align_down((sen_h - th) / 2, offy.nInc));
  MV_CC_SetIntValueEx(g_handle, "OffsetX", cx);
  MV_CC_SetIntValueEx(g_handle, "OffsetY", cy);

  /* 读回实际值 */
  MVCC_INTVALUE_EX wa{}, ha{};
  MV_CC_GetIntValueEx(g_handle, "Width", &wa);
  MV_CC_GetIntValueEx(g_handle, "Height", &ha);
  SPDLOG_INFO("camera: 分辨率设置 {}x{} -> {}x{} (off={},{})",
              (int)w.nCurValue, (int)h.nCurValue, (int)wa.nCurValue,
              (int)ha.nCurValue, (int)cx, (int)cy);
}

bool init(int timeout_ms) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_ready) return true;
  if (timeout_ms <= 0) timeout_ms = config::g.camera_timeout_ms;
  (void)timeout_ms;

  /* 配置 eth1 IP */
  std::string ip_cmd =
      "ip addr show " + config::g.camera_iface + " | grep -q " +
      config::g.camera_ip.substr(0, config::g.camera_ip.find('/')) +
      " || ip addr add " + config::g.camera_ip + " dev " +
      config::g.camera_iface + " 2>/dev/null";
  system(ip_cmd.c_str());

  /* 发现相机 */
  SPDLOG_INFO("camera: 正在发现相机...");
  MV_CC_DEVICE_INFO_LIST devs;
  memset(&devs, 0, sizeof(devs));
  int ret = MV_CC_EnumDevices(MV_GIGE_DEVICE, &devs);
  if (ret != MV_OK || devs.nDeviceNum == 0) {
    SPDLOG_ERROR("camera: 未发现相机 (ret=0x{:x}, count={})", (unsigned)ret,
                 devs.nDeviceNum);
    return false;
  }

  MV_CC_DEVICE_INFO *dev = (MV_CC_DEVICE_INFO *)devs.pDeviceInfo[0];
  if (dev->nTLayerType == MV_GIGE_DEVICE) {
    MV_GIGE_DEVICE_INFO &gig = dev->SpecialInfo.stGigEInfo;
    unsigned ip = gig.nCurrentIp;
    SPDLOG_INFO("camera: 发现 {}  SN:{}  IP:{}.{}.{}.{}", gig.chModelName,
                gig.chSerialNumber, (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                (ip >> 8) & 0xff, ip & 0xff);
  }

  /* 创建设备并打开 */
  ret = MV_CC_CreateHandleWithoutLog(&g_handle, dev);
  if (ret != MV_OK) {
    SPDLOG_ERROR("camera: CreateHandle 失败 (0x{:x})", (unsigned)ret);
    g_handle = nullptr;
    return false;
  }
  ret = MV_CC_OpenDevice(g_handle, MV_ACCESS_Exclusive, 0);
  if (ret != MV_OK) {
    SPDLOG_ERROR("camera: OpenDevice 失败 (0x{:x})", (unsigned)ret);
    MV_CC_DestroyHandle(g_handle);
    g_handle = nullptr;
    return false;
  }

  /* GigE 最优包长(提升传输带宽利用率) */
  int pkt = MV_CC_GetOptimalPacketSize(g_handle);
  if (pkt > 0) MV_CC_SetIntValueEx(g_handle, "GevSCPSPacketSize", pkt);

  MV_CC_SetEnumValue(g_handle, "ExposureAuto", 2);
  MV_CC_SetEnumValue(g_handle, "GainAuto", 2);

  /* 显式设置采集分辨率：默认(0)复位为传感器原生满分辨率，覆盖相机残留的脏配置 */
  apply_roi();

  /* Latest 策略：grab() 总返回最新帧并丢弃旧帧，避免慢消费时帧在缓冲区
   * 排队造成 UI 画面滞后。缓冲数降到 2 进一步压低排队深度。 */
  MV_CC_SetGrabStrategy(g_handle, MV_GrabStrategy_LatestImages);
  MV_CC_SetImageNodeNum(g_handle, 2);

  ret = MV_CC_StartGrabbing(g_handle);
  if (ret != MV_OK) {
    SPDLOG_ERROR("camera: StartGrabbing 失败 (0x{:x})", (unsigned)ret);
    MV_CC_CloseDevice(g_handle);
    MV_CC_DestroyHandle(g_handle);
    g_handle = nullptr;
    return false;
  }

  /* 取第一帧获取分辨率和像素格式 */
  MV_FRAME_OUT frame;
  memset(&frame, 0, sizeof(frame));
  ret = MV_CC_GetImageBuffer(g_handle, &frame, 5000);
  if (ret != MV_OK) {
    SPDLOG_ERROR("camera: 首帧抓取失败 (0x{:x})", (unsigned)ret);
    MV_CC_StopGrabbing(g_handle);
    MV_CC_CloseDevice(g_handle);
    MV_CC_DestroyHandle(g_handle);
    g_handle = nullptr;
    return false;
  }

  g_w = frame.stFrameInfo.nWidth;
  g_h = frame.stFrameInfo.nHeight;
  g_pixel_type = frame.stFrameInfo.enPixelType;  /* 记录原生像素格式 */
  MV_CC_FreeImageBuffer(g_handle, &frame);

  g_ready = true;
  SPDLOG_INFO("camera: 就绪 {}x{} pixelType=0x{:x} (via MVS SDK aarch64)",
              g_w, g_h, (unsigned)g_pixel_type);
  return true;
}

bool is_ready() { return g_ready; }

bool grab(cv::Mat &out) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (!g_ready || !g_handle) return false;

  MV_FRAME_OUT frame;
  memset(&frame, 0, sizeof(frame));
  int ret = MV_CC_GetImageBuffer(g_handle, &frame, 5000);
  if (ret != MV_OK) return false;

  int w = frame.stFrameInfo.nWidth;
  int h = frame.stFrameInfo.nHeight;
  const uchar *src = frame.pBufAddr;

  /* 根据相机像素格式转 BGR(Mono→灰度复制后转 BGR；Bayer/RGB 用 OpenCV 转换)
   * 注意: OpenCV 的 Bayer 命名按第二行像素, 与相机命名互为对调。 */
  switch ((unsigned)g_pixel_type) {
    case PixelType_Gvsp_BGR8_Packed:
      out = cv::Mat(h, w, CV_8UC3, (void *)src).clone();
      break;
    case PixelType_Gvsp_RGB8_Packed:
      cv::cvtColor(cv::Mat(h, w, CV_8UC3, (void *)src), out, cv::COLOR_RGB2BGR);
      break;
    case PixelType_Gvsp_Mono8:
      cv::cvtColor(cv::Mat(h, w, CV_8UC1, (void *)src), out, cv::COLOR_GRAY2BGR);
      break;
    case PixelType_Gvsp_BayerRG8:  /* RGGB */
      cv::cvtColor(cv::Mat(h, w, CV_8UC1, (void *)src), out, cv::COLOR_BayerBG2BGR);
      break;
    case PixelType_Gvsp_BayerBG8:  /* BGGR */
      cv::cvtColor(cv::Mat(h, w, CV_8UC1, (void *)src), out, cv::COLOR_BayerRG2BGR);
      break;
    case PixelType_Gvsp_BayerGR8:  /* GRBG */
      cv::cvtColor(cv::Mat(h, w, CV_8UC1, (void *)src), out, cv::COLOR_BayerGB2BGR);
      break;
    case PixelType_Gvsp_BayerGB8:  /* GBRG */
      cv::cvtColor(cv::Mat(h, w, CV_8UC1, (void *)src), out, cv::COLOR_BayerGR2BGR);
      break;
    default:
      if (!g_pixel_warned) {
        SPDLOG_WARN("camera: 不支持的像素格式 0x{:x}, 无法转换",
                    (unsigned)g_pixel_type);
        g_pixel_warned = true;
      }
      MV_CC_FreeImageBuffer(g_handle, &frame);
      return false;
  }

  MV_CC_FreeImageBuffer(g_handle, &frame);
  return true;
}

void shutdown() {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (g_handle) {
    MV_CC_StopGrabbing(g_handle);
    MV_CC_CloseDevice(g_handle);
    MV_CC_DestroyHandle(g_handle);
    g_handle = nullptr;
  }
  g_ready = false;
  g_w = g_h = 0;
  g_pixel_warned = false;
  SPDLOG_INFO("camera: 已关闭");
}

}  // namespace camera_capture
