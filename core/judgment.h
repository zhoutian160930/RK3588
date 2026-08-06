#pragma once
#include <vector>
#include "common.h"

/* 单个盒子的判定结果 */
struct BoxJudgment {
  int box_left = 0, box_right = 0, box_top = 0, box_bottom = 0;
  bool revealed = false;   /* 是否在两竖线内(完全漏出) */
  int material_count = 0;  /* 盒内物料数 */
  bool full = false;       /* 是否满料(仅对 revealed 的盒子有意义) */
};

/* 多盒汇总 */
struct JudgeSummary {
  int total = 0, full = 0, not_full = 0, not_revealed = 0;
};

/* 多盒判定：找出所有盒子(box_class)，每个判断是否在两竖线内；
 * 物料(material_class)按中心点归属到包含它的盒子。
 * line_left_frac/right 为竖线占图宽比例(0~1)。 */
std::vector<BoxJudgment> judge_all_boxes(const object_detect_result_list &res,
                                         int orig_w, float line_left_frac,
                                         float line_right_frac, int target,
                                         int box_class, int material_class);

/* 汇总：满/未满/未漏出 的盒子数 */
JudgeSummary summarize(const std::vector<BoxJudgment> &boxes);

/* 本帧是否合格：检测区内有满料盒且无未满盒 */
inline bool is_qualified(const JudgeSummary &sum) {
  return sum.full > 0 && sum.not_full == 0;
}
