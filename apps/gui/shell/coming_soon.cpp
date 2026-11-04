#include "shell/coming_soon.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "shell/feature_badge.hpp"
#include "widgets.hpp"

namespace p3dgui {

ComingSoonPanel::ComingSoonPanel(const QString& title, const QString& what, const QStringList& needs, QWidget* parent)
    : QWidget(parent) {
  setObjectName("ComingSoonPanel");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(48, 48, 48, 48);
  outer->addStretch(1);
  auto* card = new Card(this);
  card->setMaximumWidth(560);
  auto* col = new QVBoxLayout(card);
  col->setContentsMargins(32, 28, 32, 28);
  col->setSpacing(12);
  auto* head = new QHBoxLayout;
  head->setSpacing(12);
  head->addWidget(make_label(title, "display"));
  head->addWidget(make_feature_badge(Feature::ComingSoon), 0, Qt::AlignVCenter);
  head->addStretch(1);
  col->addLayout(head);
  auto* body = make_label(what, "lead");
  body->setWordWrap(true);
  col->addWidget(body);
  if (!needs.isEmpty()) {
    col->addSpacing(4);
    col->addWidget(make_label(tr("What it will need"), "section"));
    for (const QString& n : needs) {
      auto* l = make_label(QStringLiteral("•  ") + n, "secondary");
      l->setWordWrap(true);
      col->addWidget(l);
    }
  }
  auto* row = new QHBoxLayout;
  row->addStretch(1);
  row->addWidget(card, 10);
  row->addStretch(1);
  outer->addLayout(row);
  outer->addStretch(2);
}

}  // namespace p3dgui
