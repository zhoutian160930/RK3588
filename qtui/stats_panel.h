#pragma once
/* 生产统计面板:正确/错误计数、当前帧判定、状态行、目标物料数、文件信息 */

#include <QWidget>

class QLabel;
class QSpinBox;

class StatsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit StatsPanel(QWidget *parent = nullptr);

  void setCounts(int correct, int wrong);
  /* 判定结果:total=0 显示"未检测到盒子";否则按满/未满/未漏出计数着色 */
  void setJudge(int total, int full, int not_full, int not_revealed);
  /* total<0: 等待画面 */
  void setJudgeWaiting();
  void setStatus(const QString &st);
  int targetCount() const;
  void setTargetCount(int v);

  void setYoloFile(const QString &name);
  void setLabelFile(const QString &name);
  void setInputFile(const QString &name);

 signals:
  void targetChanged(int v); /* +/− 或直接输入,主窗口写 config */

 private:
  QLabel *stat_label_ = nullptr;   /* 正确/错误 */
  QLabel *judge_label_ = nullptr;  /* 当前帧判定 */
  QLabel *status_label_ = nullptr; /* 状态行 */
  QSpinBox *target_spin_ = nullptr;
  QLabel *yolo_val_ = nullptr;
  QLabel *label_val_ = nullptr;
  QLabel *input_val_ = nullptr;
};
