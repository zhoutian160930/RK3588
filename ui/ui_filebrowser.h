#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ui_fb_selected_cb)(const char *path, void *user_data);

/* 弹出模态文件浏览器。
 * start_dir: 起始目录
 * filter_exts: 允许选择的文件后缀(逗号分隔, 如 ".rknn" 或 ".jpg,.png,.mp4")，空串=所有文件
 * allow_dir_select: 是否允许把"当前目录"作为选择对象(输入源用)
 * cb: 选中文件/目录时的回调(传入完整路径)
 * user_data: 透传给回调 */
void ui_filebrowser_open(const char *start_dir, const char *filter_exts,
                         int allow_dir_select, ui_fb_selected_cb cb,
                         void *user_data);

#ifdef __cplusplus
}
#endif
