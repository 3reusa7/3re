#pragma once
// A workspace that is not available yet: what it will do and what it will need.
// Deliberately has no buttons, so nothing looks usable that is not.
#include <QStringList>
#include <QWidget>

namespace p3dgui {

class ComingSoonPanel : public QWidget {
  Q_OBJECT
 public:
  ComingSoonPanel(const QString& title, const QString& what, const QStringList& needs, QWidget* parent = nullptr);
};

}  // namespace p3dgui
