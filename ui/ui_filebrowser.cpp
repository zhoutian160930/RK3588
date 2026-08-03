#include "ui_filebrowser.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

struct FbCtx {
  std::string cur;
  std::string filter;
  ui_fb_selected_cb cb;
  void *user_data;
  int allow_dir;
  lv_obj_t *win;
  lv_obj_t *list;
  lv_obj_t *path_label;
};

static FbCtx *g_ctx = NULL;

static bool has_ext(const std::string &name, const std::string &filter) {
  if (filter.empty()) return true;
  std::string n = name;
  for (auto &c : n) c = (char)tolower(c);
  size_t i = 0;
  while (i < filter.size()) {
    size_t j = filter.find(',', i);
    std::string e = (j == std::string::npos) ? filter.substr(i)
                                              : filter.substr(i, j - i);
    for (auto &c : e) c = (char)tolower(c);
    if (!e.empty() && n.size() >= e.size() &&
        n.compare(n.size() - e.size(), e.size(), e) == 0)
      return true;
    if (j == std::string::npos) break;
    i = j + 1;
  }
  return false;
}

static void reload_list();

static void close_browser() {
  if (g_ctx) {
    lv_obj_del(g_ctx->win);
    delete g_ctx;
    g_ctx = NULL;
  }
}

static void on_item(lv_event_t *e) {
  if (!g_ctx) return;
  /* 用 current_target 取回调所属的按钮(无论点中的是图标还是文字子对象) */
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
  /* lv_list_add_btn 先放图标(image)再放文字(label)，所以文字是最后一个子对象。
   * 不能用 child(0)——那是图标，lv_label_get_text 会返回图标的符号字节污染路径。 */
  uint32_t cnt = lv_obj_get_child_cnt(btn);
  lv_obj_t *label = cnt ? lv_obj_get_child(btn, cnt - 1) : NULL;
  if (!label) return;
  std::string name = lv_label_get_text(label);

  if (name == "..") {
    size_t p = g_ctx->cur.find_last_of('/');
    g_ctx->cur = (p == std::string::npos || p == 0) ? "/" : g_ctx->cur.substr(0, p);
    reload_list();
    return;
  }
  std::string full = g_ctx->cur + "/" + name;
  struct stat st;
  if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    g_ctx->cur = full;
    reload_list();
    return;
  }
  if (g_ctx->cb) g_ctx->cb(full.c_str(), g_ctx->user_data);
  close_browser();
}

static void on_select_dir(lv_event_t *e) {
  LV_UNUSED(e);
  if (g_ctx && g_ctx->allow_dir && g_ctx->cb)
    g_ctx->cb(g_ctx->cur.c_str(), g_ctx->user_data);
  close_browser();
}

static void on_close(lv_event_t *e) {
  LV_UNUSED(e);
  close_browser();
}

static void reload_list() {
  if (!g_ctx) return;
  lv_obj_clean(g_ctx->list);
  lv_label_set_text(g_ctx->path_label, g_ctx->cur.c_str());
  DIR *d = opendir(g_ctx->cur.empty() ? "/" : g_ctx->cur.c_str());
  if (!d) return;
  std::vector<std::string> dirs, files;
  struct dirent *ent;
  while ((ent = readdir(d))) {
    std::string n = ent->d_name;
    if (n == ".") continue;
    if (g_ctx->cur == "/" && n == "..") continue;
    std::string full = g_ctx->cur + "/" + n;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode))
      dirs.push_back(n);
    else if (has_ext(n, g_ctx->filter))
      files.push_back(n);
  }
  closedir(d);
  std::sort(dirs.begin(), dirs.end());
  std::sort(files.begin(), files.end());
  lv_obj_t *b;
  b = lv_list_add_btn(g_ctx->list, LV_SYMBOL_UP, "..");
  lv_obj_add_event_cb(b, on_item, LV_EVENT_CLICKED, NULL);
  for (auto &n : dirs) {
    b = lv_list_add_btn(g_ctx->list, LV_SYMBOL_DIRECTORY, n.c_str());
    lv_obj_add_event_cb(b, on_item, LV_EVENT_CLICKED, NULL);
  }
  for (auto &n : files) {
    b = lv_list_add_btn(g_ctx->list, LV_SYMBOL_FILE, n.c_str());
    lv_obj_add_event_cb(b, on_item, LV_EVENT_CLICKED, NULL);
  }
}

void ui_filebrowser_open(const char *start_dir, const char *filter_exts,
                         int allow_dir_select, ui_fb_selected_cb cb,
                         void *user_data) {
  if (g_ctx) close_browser();
  g_ctx = new FbCtx();
  g_ctx->cur = (start_dir && *start_dir) ? start_dir : "/";
  g_ctx->filter = filter_exts ? filter_exts : "";
  g_ctx->cb = cb;
  g_ctx->user_data = user_data;
  g_ctx->allow_dir = allow_dir_select;

  g_ctx->win = lv_obj_create(lv_scr_act());
  lv_obj_set_size(g_ctx->win, 620, 520);
  lv_obj_center(g_ctx->win);
  lv_obj_set_style_bg_color(g_ctx->win, lv_color_white(), 0);
  lv_obj_set_style_border_width(g_ctx->win, 2, 0);
  lv_obj_clear_flag(g_ctx->win, LV_OBJ_FLAG_SCROLLABLE);

  g_ctx->path_label = lv_label_create(g_ctx->win);
  lv_obj_align(g_ctx->path_label, LV_ALIGN_TOP_LEFT, 10, 10);
  lv_obj_set_style_text_color(g_ctx->path_label, lv_color_black(), 0);

  lv_obj_t *close_btn = lv_btn_create(g_ctx->win);
  lv_obj_set_size(close_btn, 50, 36);
  lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -10, 6);
  lv_obj_t *cl = lv_label_create(close_btn);
  lv_label_set_text(cl, "X");
  lv_obj_center(cl);
  lv_obj_add_event_cb(close_btn, on_close, LV_EVENT_CLICKED, NULL);

  if (allow_dir_select) {
    lv_obj_t *dir_btn = lv_btn_create(g_ctx->win);
    lv_obj_set_size(dir_btn, 140, 36);
    lv_obj_align(dir_btn, LV_ALIGN_TOP_RIGHT, -70, 6);
    lv_obj_t *dl = lv_label_create(dir_btn);
    lv_label_set_text(dl, "选此目录");
    lv_obj_center(dl);
    lv_obj_add_event_cb(dir_btn, on_select_dir, LV_EVENT_CLICKED, NULL);
  }

  g_ctx->list = lv_list_create(g_ctx->win);
  lv_obj_set_size(g_ctx->list, 590, 430);
  lv_obj_align(g_ctx->list, LV_ALIGN_BOTTOM_MID, 0, -10);
  reload_list();
}
