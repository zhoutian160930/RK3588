#include "judgment.h"

std::vector<BoxJudgment> judge_all_boxes(const object_detect_result_list &res,
                                         int orig_w, float line_left_frac,
                                         float line_right_frac, int target,
                                         int box_class, int material_class) {
  std::vector<BoxJudgment> boxes;
  int ll = (int)(line_left_frac * orig_w);
  int rr = (int)(line_right_frac * orig_w);
  for (int i = 0; i < res.count; i++) {
    if (res.results[i].cls_id != box_class) continue;
    BoxJudgment b;
    b.box_left = res.results[i].box.left;
    b.box_right = res.results[i].box.right;
    b.box_top = res.results[i].box.top;
    b.box_bottom = res.results[i].box.bottom;
    b.revealed = (b.box_left >= ll) && (b.box_right <= rr);
    boxes.push_back(b);
  }
  for (int i = 0; i < res.count; i++) {
    if (res.results[i].cls_id != material_class) continue;
    int cx = (res.results[i].box.left + res.results[i].box.right) / 2;
    int cy = (res.results[i].box.top + res.results[i].box.bottom) / 2;
    for (auto &b : boxes) {
      if (cx >= b.box_left && cx <= b.box_right && cy >= b.box_top &&
          cy <= b.box_bottom) {
        b.material_count++;
        break;
      }
    }
  }
  for (auto &b : boxes) {
    if (b.revealed) b.full = (b.material_count == target);
  }
  return boxes;
}

JudgeSummary summarize(const std::vector<BoxJudgment> &boxes) {
  JudgeSummary s;
  s.total = boxes.size();
  for (auto &b : boxes) {
    if (!b.revealed) s.not_revealed++;
    else if (b.full) s.full++;
    else s.not_full++;
  }
  return s;
}
