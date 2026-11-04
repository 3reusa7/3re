#pragma once
// Small reusable building blocks styled by the design system (theme.hpp).
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <optional>

#include "icons.hpp"
#include "theme.hpp"

class QHBoxLayout;
class QVBoxLayout;
class QVariantAnimation;

namespace p3dgui {

// A label with a type role (small, muted, section, title, status, ...).
QLabel* make_label(const QString& text, const char* type = nullptr, QWidget* parent = nullptr);
// Buttons: kind = "primary", "ghost", "link", "row", "segment" or nullptr (default).
QPushButton* make_button(const QString& text, const char* kind = nullptr, const QIcon& icon = {},
                         QWidget* parent = nullptr);
// Icon-only tool button bound to an action (tooltip = action text + shortcut).
QToolButton* make_tool_button(QAction* action, QWidget* parent = nullptr, int icon_px = theme::icon_size::md);
// A 1 px divider line (horizontal), and a vertical one with space around it for toolbars.
QFrame* make_divider(QWidget* parent = nullptr);
QWidget* make_vdivider(int hmargin = 8, int vmargin = 12);
// Tone chip ("PASS", "Estimated", ...).
QLabel* make_chip(const QString& text, theme::Tone tone, QWidget* parent = nullptr);
void set_chip(QLabel* chip, const QString& text, theme::Tone tone);

// Collapsible section: a header row (chevron, caps title, optional trailing
// widget) over a body. Expanding animates the height (160 ms).
class Section : public QWidget {
  Q_OBJECT
 public:
  explicit Section(const QString& title, QWidget* parent = nullptr, bool expanded = true);
  QVBoxLayout* body() const { return body_layout_; }
  void setTitle(const QString& t);
  void setTrailing(QWidget* w);
  void setExpanded(bool on, bool animate = true);
  bool expanded() const { return expanded_; }
  void setCollapsible(bool on);

 signals:
  void toggled(bool expanded);

 private:
  QPushButton* header_ = nullptr;
  QWidget* trailing_host_ = nullptr;
  QWidget* body_ = nullptr;
  QVBoxLayout* body_layout_ = nullptr;
  QVariantAnimation* anim_ = nullptr;
  bool expanded_ = true;
  bool collapsible_ = true;
};

// Minimal dock title bar: caps title + optional trailing widgets + hide button.
class DockTitle : public QWidget {
  Q_OBJECT
 public:
  DockTitle(const QString& title, QWidget* parent = nullptr);
  void addTrailing(QWidget* w);

 signals:
  void closeRequested();

 private:
  QHBoxLayout* row_ = nullptr;
};

// Rounded card with an optional 3 px tone edge on the left.
class Card : public QFrame {
  Q_OBJECT
 public:
  explicit Card(QWidget* parent = nullptr);
  void setTone(std::optional<theme::Tone> t);

 protected:
  void paintEvent(QPaintEvent* e) override;

 private:
  std::optional<theme::Tone> tone_;
};

}  // namespace p3dgui
