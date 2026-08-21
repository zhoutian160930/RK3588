#include "log_panel.h"

#include <QPlainTextEdit>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {
constexpr int kMaxBlocks = 400; /* 每视图最大行数(超出删最旧) */
}

LogPanel::LogPanel(QWidget *parent) : QWidget(parent) {
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  tabs_ = new QTabWidget(this);
  lay->addWidget(tabs_);

  auto mk_view = [&] {
    auto *v = new QPlainTextEdit(this);
    v->setReadOnly(true);
    v->setMaximumBlockCount(kMaxBlocks); /* Qt 自带行数上限+自动裁剪 */
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
  };
  info_view_ = mk_view();
  log_view_ = mk_view();
  warn_view_ = mk_view();

  tabs_->addTab(info_view_, QStringLiteral("物料信息"));
  tabs_->addTab(log_view_, QStringLiteral("运行日志"));
  tabs_->addTab(warn_view_, QStringLiteral("警告和报警"));

  connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
    emit visibilityChanged(true);
  });
}

void LogPanel::appendMaterialInfo(const QString &line) {
  info_view_->appendPlainText(line);
}

void LogPanel::appendLogInfo(const QStringList &lines) {
  for (const auto &l : lines) log_view_->appendPlainText(l);
}

void LogPanel::appendLogDbgErr(const QStringList &lines) {
  for (const auto &l : lines) warn_view_->appendPlainText(l);
}
