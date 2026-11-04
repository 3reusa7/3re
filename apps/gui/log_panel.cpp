#include "log_panel.hpp"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include "qt_util.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;

LogPanel::LogPanel(QWidget* parent) : QWidget(parent) {
  setProperty("surface", "panel");
  setAttribute(Qt::WA_StyledBackground, true);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(theme::space::lg, theme::space::xs, theme::space::lg, theme::space::md);
  lay->setSpacing(theme::space::sm);
  auto* split = new QSplitter(Qt::Horizontal);
  auto column = [&](const QString& title, QPlainTextEdit*& edit) {
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(theme::space::xs);
    v->addWidget(make_label(title, "section"));
    edit = new QPlainTextEdit;
    edit->setReadOnly(true);
    edit->setFont(theme::font(theme::Type::Mono));
    edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    v->addWidget(edit, 1);
    split->addWidget(w);
  };
  column(tr("Last operation"), last_);
  column(tr("History"), log_);
  split->setStretchFactor(0, 1);
  split->setStretchFactor(1, 1);
  lay->addWidget(split, 1);
  auto* row = new QHBoxLayout;
  row->addStretch();
  auto* copy = make_button(tr("Copy"), "ghost");
  connect(copy, &QPushButton::clicked, this,
          [this] { QApplication::clipboard()->setText(last_->toPlainText() + "\n" + log_->toPlainText()); });
  row->addWidget(copy);
  lay->addLayout(row);
}

QString LogPanel::lastText() const { return last_->toPlainText(); }

void LogPanel::update_from(const SessionView& v) {
  QString t;
  for (const LogEntry& e : v.log)
    t += QStringLiteral("%1  %2  %3  %4\n").arg(qs(e.time), qs(to_string(e.source)), qs(e.name), qs(e.summary));
  log_->setPlainText(t);
  log_->moveCursor(QTextCursor::End);
}

void LogPanel::showResult(const QString& op, const OpResult& r) {
  QString t = tr("%1: %2 — status %3, revision %4\n")
                  .arg(op, r.success ? tr("ok") : tr("FAILED"), qs(to_string(r.status)))
                  .arg(r.revision);
  for (const auto* list : {&r.errors, &r.warnings, &r.info})
    for (const Message& m : *list) t += QStringLiteral("[%1] %2: %3\n").arg(qs(to_string(m.severity)), qs(m.code), qs(m.text));
  last_->setPlainText(t);
}

}  // namespace p3dgui
