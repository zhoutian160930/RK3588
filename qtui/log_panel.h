#pragma once
/* 底部信息区:三个标签页 — 物料信息 / 运行日志 / 警告和报警 */

#include <QWidget>

class QPlainTextEdit;
class QTabWidget;

class LogPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LogPanel(QWidget *parent = nullptr);

  /* 物料信息追加一行(自动滚动到底) */
  void appendMaterialInfo(const QString &line);
  /* 批量追加日志(运行日志 / 警告报警 两路) */
  void appendLogInfo(const QStringList &lines);
  void appendLogDbgErr(const QStringList &lines);

 signals:
  void visibilityChanged(bool visible); /* 供暂停高频刷新 */

 private:
  QTabWidget *tabs_ = nullptr;
  QPlainTextEdit *info_view_ = nullptr;
  QPlainTextEdit *log_view_ = nullptr;
  QPlainTextEdit *warn_view_ = nullptr;

  void trimAndScroll(QPlainTextEdit *v);
};
