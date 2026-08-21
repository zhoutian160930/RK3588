#!/bin/bash
# 桌面以 root 启动检测程序(GPIO sysfs 输出需要 root)。
# 被 ~/.local/share/applications/yolov8-detection.desktop 调用。
# 一次性配置(免密;目标不能带 '.',sudo 忽略带点文件名):
#   sudo cp scripts/90-yolov8-root.sudoers /etc/sudoers.d/90-yolov8-root
#   sudo chmod 440 /etc/sudoers.d/90-yolov8-root
BIN=/home/forlinx/lvgl/RK3588/build/yolov8_sam_demo_qt
OUT_DIR=/home/forlinx/lvgl/output
CHOWN_CMD=(/usr/bin/chown -R forlinx:forlinx "$OUT_DIR")

# root 运行期间产生的文件(detImg/saveImg/log)属主为 root,归还给普通用户:
# 启动前修复上次残留,退出后修复本次产物,避免文件管理器删不动/普通用户写不进。
chown_outputs() {
  if [ "$(id -u)" = "0" ]; then
    "${CHOWN_CMD[@]}" 2>/dev/null
  else
    sudo -n "${CHOWN_CMD[@]}" 2>/dev/null
  fi
  return 0
}

chown_outputs                         # 修复上次运行残留

if [ "$(id -u)" = "0" ]; then
  exec "$BIN"                         # pkexec root 路径:退出无法回到脚本,
fi                                    # 属主由下次启动前的 chown 兜底

# 允许 root 访问当前 X 显示(root 下 GUI 需要)
xhost +SI:localuser:root >/dev/null 2>&1

# 首选: sudoers 免密(sudo 前台等待程序退出);失败则回退 pkexec 图形密码框
if sudo -n "$BIN"; then
  chown_outputs                       # 程序退出后归还本次产物属主
else
  pkexec "$BIN"                       # 此分支退出后无法 chown,下次启动兜底
fi
