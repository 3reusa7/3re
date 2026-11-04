#pragma once
// The 3re workspace rail: one entry per workspace (icon above label), exactly one current.
#include <QIcon>
#include <QMap>
#include <QWidget>

class QButtonGroup;
class QToolButton;
class QVBoxLayout;

namespace p3dgui {

class NavRail : public QWidget {
  Q_OBJECT
 public:
  explicit NavRail(QWidget* parent = nullptr);
  int addItem(const QIcon& icon, const QString& label, const QString& id);
  void addStretch();
  void setCurrent(const QString& id);
  QString current() const { return current_; }
  QToolButton* button(const QString& id) const { return buttons_.value(id); }

 signals:
  void selected(const QString& id);

 private:
  QVBoxLayout* layout_ = nullptr;
  QButtonGroup* group_ = nullptr;
  QMap<QString, QToolButton*> buttons_;
  QString current_;
};

}  // namespace p3dgui
