#include "stats_panel.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

StatsPanel::StatsPanel(QWidget *parent) : QWidget(parent) {
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(8, 8, 8, 8);
  lay->setSpacing(6);

  auto *title = new QLabel(QStringLiteral("生产统计"), this);
  QFont tf = title->font();
  tf.setBold(true);
  title->setFont(tf);
  lay->addWidget(title, 0, Qt::AlignHCenter);

  stat_label_ = new QLabel(this);
  setCounts(0, 0);
  lay->addWidget(stat_label_);

  judge_label_ = new QLabel(QStringLiteral("等待..."), this);
  judge_label_->setWordWrap(true);
  lay->addWidget(judge_label_);

  status_label_ = new QLabel(QStringLiteral("状态: 待机"), this);
  lay->addWidget(status_label_);

  auto *tgt_row = new QHBoxLayout();
  auto *tgt_title = new QLabel(QStringLiteral("目标物料数:"), this);
  target_spin_ = new QSpinBox(this);
  target_spin_->setRange(1, 999);
  target_spin_->setValue(10);
  connect(target_spin_,
          static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this,
          &StatsPanel::targetChanged);
  tgt_row->addWidget(tgt_title);
  tgt_row->addWidget(target_spin_, 0, Qt::AlignLeft);
  tgt_row->addStretch();
  lay->addLayout(tgt_row);

  auto mk_file = [&](const char *prefix, QLabel *&val) {
    auto *row = new QHBoxLayout();
    auto *p = new QLabel(QString::fromUtf8(prefix), this);
    val = new QLabel(this);
    val->setMinimumWidth(120);
    p->setFixedWidth(80);
    row->addWidget(p);
    row->addWidget(val, 1);
    lay->addLayout(row);
  };
  mk_file("模型文件:", yolo_val_);
  mk_file("标签文件:", label_val_);
  mk_file("输入源:", input_val_);
  lay->addStretch();
}

void StatsPanel::setCounts(int correct, int wrong) {
  stat_label_->setText(QString("<span style='color:#00c000'>正确: %1</span>"
                               "&nbsp;&nbsp;&nbsp;"
                               "<span style='color:#ff3030'>错误: %2</span>")
                           .arg(correct)
                           .arg(wrong));
}

void StatsPanel::setJudge(int total, int full, int not_full, int not_revealed) {
  QString text = QString(QStringLiteral("共%1盒:满 %2 未满 %3 未漏出 %4"))
                     .arg(total)
                     .arg(full)
                     .arg(not_full)
                     .arg(not_revealed);
  judge_label_->setText(text);
  QString color = (not_full > 0)     ? "#ff3030"
                  : (not_revealed > 0) ? "#ffa500"
                                       : "#00c000";
  judge_label_->setStyleSheet(QString("color:%1;").arg(color));
}

void StatsPanel::setJudgeWaiting() {
  judge_label_->setText(QStringLiteral("等待画面..."));
  judge_label_->setStyleSheet("");
}

void StatsPanel::setStatus(const QString &st) { status_label_->setText(st); }

int StatsPanel::targetCount() const { return target_spin_->value(); }

void StatsPanel::setTargetCount(int v) { target_spin_->setValue(v); }

void StatsPanel::setYoloFile(const QString &name) {
  yolo_val_->setText(name);
  yolo_val_->setToolTip(name);
}
void StatsPanel::setLabelFile(const QString &name) {
  label_val_->setText(name);
  label_val_->setToolTip(name);
}
void StatsPanel::setInputFile(const QString &name) {
  input_val_->setText(name);
  input_val_->setToolTip(name);
}
