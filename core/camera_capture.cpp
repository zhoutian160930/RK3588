#include "camera_capture.h"
#include "config.h"

#include <opencv2/imgproc.hpp>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace camera_capture {

static int g_w = 0, g_h = 0;
static unsigned long long g_last_fid = 0;
static std::vector<uint8_t> g_buf;
static bool g_ready = false;

/* 启动 grab_stream（通过 qemu 模拟 x86） */
static bool start_grab_stream() {
  system("pkill -f grab_stream 2>/dev/null");
  usleep(200000);
  unlink("/tmp/camera_info.txt");
  unlink("/tmp/camera_frame.raw");
  unlink("/tmp/camera_frame.tmp");

  /* 配置 eth1 IP */
  std::string ip_cmd = "ip addr show " + config::g.camera_iface +
                       " | grep -q " + config::g.camera_ip.substr(0, config::g.camera_ip.find('/')) +
                       " || ip addr add " + config::g.camera_ip + " dev " + config::g.camera_iface +
                       " 2>/dev/null";
  system(ip_cmd.c_str());

  /* 启动 grab_stream */
  std::string cmd = "LD_LIBRARY_PATH=" + config::g.camera_lib_path + " " +
                     config::g.camera_qemu_bin + " " + config::g.camera_grab_bin +
                     " &>/dev/null &";
  int ret = system(cmd.c_str());
  if (ret != 0) {
    SPDLOG_ERROR("camera: 启动 grab_stream 失败");
    return false;
  }
  SPDLOG_INFO("camera: grab_stream 已启动 ({} via {})", config::g.camera_grab_bin,
              config::g.camera_qemu_bin);
  return true;
}

bool init(int timeout_ms) {
  if (g_ready) return true; /* 已就绪，幂等返回 */
  if (timeout_ms <= 0) timeout_ms = config::g.camera_timeout_ms;
  if (!start_grab_stream()) return false;

  SPDLOG_INFO("camera: 等待相机就绪...");
  int waited = 0;
  while (g_w == 0 || g_h == 0) {
    std::ifstream f("/tmp/camera_info.txt");
    if (f >> g_w >> g_h) break;
    f.close();
    if (timeout_ms > 0 && waited >= timeout_ms) {
      SPDLOG_ERROR("camera: 等待相机超时({}ms)，检查 {} 网络和相机连接",
                   timeout_ms, config::g.camera_iface);
      return false;
    }
    usleep(500000);
    waited += 500;
  }
  if (g_w <= 0 || g_h <= 0) { SPDLOG_ERROR("camera: 分辨率异常"); return false; }

  g_buf.resize((size_t)g_w * g_h);
  g_last_fid = 0;
  g_ready = true;
  SPDLOG_INFO("camera: 就绪 {}x{}", g_w, g_h);
  return true;
}

bool is_ready() { return g_ready; }

bool grab(cv::Mat &out) {
  if (!g_ready) return false;
  FILE *fp = fopen("/tmp/camera_frame.raw", "rb");
  if (!fp) return false;
  unsigned long long fw, fh, fid;
  if (fread(&fw, sizeof(fw), 1, fp) != 1 ||
      fread(&fh, sizeof(fh), 1, fp) != 1 ||
      fread(&fid, sizeof(fid), 1, fp) != 1) { fclose(fp); return false; }
  if ((int)fw != g_w || (int)fh != g_h || fid <= g_last_fid) { fclose(fp); return false; }
  size_t sz = (size_t)g_w * g_h;
  if (fread(g_buf.data(), 1, sz, fp) != sz) { fclose(fp); return false; }
  fclose(fp);
  g_last_fid = fid;
  cv::Mat gray(g_h, g_w, CV_8UC1, g_buf.data());
  cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);
  return true;
}

void shutdown() {
  g_ready = false; g_w = g_h = 0; g_buf.clear(); g_buf.shrink_to_fit();
  system("pkill -f grab_stream 2>/dev/null");
  usleep(200000);
  unlink("/tmp/camera_info.txt");
  unlink("/tmp/camera_frame.raw");
  unlink("/tmp/camera_frame.tmp");
  SPDLOG_INFO("camera: 已关闭");
}

}  // namespace camera_capture
