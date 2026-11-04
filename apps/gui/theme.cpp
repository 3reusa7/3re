#include "theme.hpp"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

namespace p3dgui::theme {

namespace {

QString css(const QColor& c) {
  if (c.alpha() == 255) return c.name();
  return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

QString ui_family() {
  const QStringList families = QFontDatabase::families();
  for (const char* f : {"Segoe UI Variable Text", "Segoe UI", "Inter", "Noto Sans", "DejaVu Sans"})
    if (families.contains(QLatin1String(f))) return QLatin1String(f);
  return QApplication::font().family();
}

QString mono_family() {
  const QStringList families = QFontDatabase::families();
  for (const char* f : {"Cascadia Mono", "Consolas", "DejaVu Sans Mono"})
    if (families.contains(QLatin1String(f))) return QLatin1String(f);
  return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

// Small images the stylesheet needs (check mark, radio dot, arrows), drawn
// once per run at 1x/2x/3x into a cache folder; QIcon picks the @Nx file for
// the display scale.
QString ui_images_dir() {
  static const QString dir = [] {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/ui-images";
    QDir().mkpath(d);
    auto write = [&](const QString& name, const QColor& col, int kind) {
      for (int scale : {1, 2, 3}) {
        const int s = 16 * scale;
        QImage img(s, s, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(scale, scale);
        QPainterPath path;
        if (kind == 0) {  // check
          p.setPen(QPen(col, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
          path.moveTo(3.8, 8.3);
          path.lineTo(6.7, 11.1);
          path.lineTo(12.2, 5.2);
          p.drawPath(path);
        } else if (kind == 1) {  // dot
          p.setPen(Qt::NoPen);
          p.setBrush(col);
          p.drawEllipse(QPointF(8, 8), 3.2, 3.2);
        } else {  // chevron down (2) / up (3)
          const double y0 = kind == 2 ? 6.2 : 9.8, y1 = kind == 2 ? 9.8 : 6.2;
          p.setPen(QPen(col, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
          path.moveTo(4.5, y0);
          path.lineTo(8, y1);
          path.lineTo(11.5, y0);
          p.drawPath(path);
        }
        p.end();
        img.save(QStringLiteral("%1/%2%3.png").arg(d, name, scale == 1 ? QString() : QStringLiteral("@%1x").arg(scale)));
      }
    };
    write("check", QColor(255, 255, 255), 0);
    write("dot", QColor(255, 255, 255), 1);
    write("down", QColor(0xa6, 0xa9, 0xb1), 2);
    write("up", QColor(0xa6, 0xa9, 0xb1), 3);
    return d;
  }();
  return dir;
}

}  // namespace

const Colors& colors() {
  static const Colors c = [] {
    Colors k;
    // 3re design system v2 (spec 2026-10-20-3re-design.md §4): soft charcoal surfaces,
    // muted slate-blue accent, muted status colours. Text pairs meet WCAG AA (>= 4.5:1).
    k.app = QColor(0x15, 0x17, 0x1c);
    k.panel = QColor(0x1b, 0x1e, 0x24);
    k.raised = QColor(0x22, 0x26, 0x2e);
    k.hover = QColor(0x2a, 0x2f, 0x38);
    k.pressed = QColor(0x31, 0x37, 0x42);
    k.input = QColor(0x17, 0x19, 0x1f);
    k.viewport_top = QColor(0x25, 0x29, 0x32);
    k.viewport_bottom = QColor(0x13, 0x15, 0x19);
    k.preview_bg = QColor(0x17, 0x19, 0x1f);
    k.checker = QColor(0x1f, 0x22, 0x29);
    k.border = QColor(0x2c, 0x31, 0x3a);
    k.border_strong = QColor(0x3a, 0x40, 0x4b);
    k.text = QColor(0xe4, 0xe7, 0xec);
    k.text_secondary = QColor(0xa3, 0xaa, 0xb6);
    k.text_muted = QColor(0x8b, 0x93, 0xa1);
    k.text_disabled = QColor(0x5a, 0x60, 0x6b);
    k.on_accent = QColor(0xff, 0xff, 0xff);
    k.accent = QColor(0x5b, 0x72, 0xa6);
    k.accent_hover = QColor(0x7c, 0x93, 0xc4);
    k.accent_pressed = QColor(0x4f, 0x65, 0x93);
    k.accent_fill_hover = QColor(0x60, 0x75, 0xa8);
    k.pass = QColor(0x5f, 0xae, 0x7a);
    k.review = QColor(0xd3, 0xa3, 0x5a);
    k.fail = QColor(0xd0, 0x6a, 0x64);
    k.info = QColor(0x6f, 0xa8, 0xb8);
    k.neutral = QColor(0x8b, 0x93, 0xa1);
    k.axis_x = QColor(0xe0, 0x5a, 0x5a);
    k.axis_y = QColor(0x6c, 0xc0, 0x5c);
    k.axis_z = QColor(0x5a, 0x8c, 0xe6);
    k.icon = QColor(0xb8, 0xbf, 0xcb);
    k.hover_tint = QColor(255, 255, 255);
    k.pixel_grid = QColor(255, 255, 255, 22);
    k.compare_match = QColor(128, 128, 128);
    k.compare_extra = QColor(230, 40, 40);
    k.compare_missing = QColor(40, 90, 255);
    k.compare_colour = QColor(255, 220, 0);
    return k;
  }();
  return c;
}

QColor tone_color(Tone t) {
  const Colors& c = colors();
  switch (t) {
    case Tone::Neutral: return c.neutral;
    case Tone::Accent: return c.accent;
    case Tone::Pass: return c.pass;
    case Tone::Review: return c.review;
    case Tone::Fail: return c.fail;
    case Tone::Info: return c.info;
  }
  return c.neutral;
}

const char* tone_name(Tone t) {
  switch (t) {
    case Tone::Neutral: return "neutral";
    case Tone::Accent: return "accent";
    case Tone::Pass: return "pass";
    case Tone::Review: return "review";
    case Tone::Fail: return "fail";
    case Tone::Info: return "info";
  }
  return "neutral";
}

Tone tone_for(pixel3d::ValidationStatus s) {
  switch (s) {
    case pixel3d::ValidationStatus::Pass: return Tone::Pass;
    case pixel3d::ValidationStatus::NeedsReview: return Tone::Review;
    case pixel3d::ValidationStatus::Fail: return Tone::Fail;
    case pixel3d::ValidationStatus::NotValidated: break;
  }
  return Tone::Neutral;
}

QColor with_alpha(QColor c, double alpha) {
  c.setAlphaF(static_cast<float>(alpha));
  return c;
}

QFont font(Type t) {
  static const QString ui = ui_family(), mono = mono_family();
  QFont f(t == Type::Mono ? mono : ui);
  switch (t) {
    case Type::Small: f.setPointSizeF(9); break;
    case Type::Body: f.setPointSizeF(10); break;
    case Type::Section:
      f.setPointSizeF(9);
      f.setWeight(QFont::DemiBold);
      f.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
      f.setCapitalization(QFont::AllUppercase);
      break;
    case Type::Title:
      f.setPointSizeF(12);
      f.setWeight(QFont::DemiBold);
      break;
    case Type::Status:
      f.setPointSizeF(15);
      f.setWeight(QFont::Bold);
      f.setLetterSpacing(QFont::AbsoluteSpacing, 0.4);
      break;
    case Type::Display:
      f.setPointSizeF(22);
      f.setWeight(QFont::DemiBold);
      break;
    case Type::Mono: f.setPointSizeF(9); break;
  }
  return f;
}

QString stylesheet() {
  const Colors& c = colors();
  QString s = QStringLiteral(R"(
QMainWindow { background: @app; }
QMainWindow::separator { background: @app; width: 4px; height: 4px; }
QWidget[surface="panel"] { background: @panel; }
QWidget[surface="app"] { background: @app; }
QWidget[surface="raised"] { background: @raised; }
QDialog, QMessageBox { background: @panel; }
QLabel { color: @text; background: transparent; }
QLabel[type="secondary"] { color: @text2; }
QLabel[type="muted"] { color: @muted; }
QLabel[type="small"] { color: @text2; font-size: 9pt; }
QLabel[type="section"] { color: @muted; font-size: 9pt; font-weight: 600; letter-spacing: 0.6px; }
QLabel[type="title"] { font-size: 12pt; font-weight: 600; }
QLabel[type="display"] { font-size: 22pt; font-weight: 600; }
QLabel[type="lead"] { color: @text2; font-size: 11pt; }
QLabel[type="status"] { font-size: 15pt; font-weight: 700; letter-spacing: 0.4px; }
QLabel[tone="pass"] { color: @pass; }
QLabel[tone="review"] { color: @review; }
QLabel[tone="fail"] { color: @fail; }
QLabel[tone="info"] { color: @info; }
QLabel[tone="accent"] { color: @accentHover; }
QLabel[tone="neutral"][type="status"] { color: @text2; }
QLabel[chip="true"] { border-radius: 4px; padding: 2px 8px; font-size: 9pt; font-weight: 600; background: @hover; color: @text2; }
QLabel[chip="true"][tone="pass"] { background: @passSoft; color: @pass; }
QLabel[chip="true"][tone="review"] { background: @reviewSoft; color: @review; }
QLabel[chip="true"][tone="fail"] { background: @failSoft; color: @fail; }
QLabel[chip="true"][tone="info"] { background: @infoSoft; color: @info; }
QLabel[chip="true"][tone="accent"] { background: @accentSoft; color: @accentHover; }

QToolTip { background: @raised; color: @text; border: 1px solid @borderStrong; padding: 6px 8px; border-radius: 6px; }

QMenuBar { background: @app; color: @text2; padding: 2px 6px; }
QMenuBar::item { padding: 6px 10px; border-radius: 4px; background: transparent; }
QMenuBar::item:selected { background: @hover; color: @text; }
QMenuBar::item:pressed { background: @pressed; color: @text; }
QMenu { background: @raised; border: 1px solid @borderStrong; padding: 6px; border-radius: 8px; }
QMenu::item { padding: 6px 32px 6px 12px; border-radius: 4px; color: @text; background: transparent; }
QMenu::item:selected { background: @accentSoft; }
QMenu::item:disabled { color: @disabled; }
QMenu::separator { height: 1px; background: @border; margin: 6px 6px; }
QMenu::icon { padding-left: 10px; }

QToolBar#MainToolbar { background: @app; border: none; border-bottom: 1px solid @border; padding: 6px 10px; spacing: 2px; }
QToolBar#MainToolbar QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 5px 6px 4px 6px; color: @text2; min-width: 58px; }
QToolBar#MainToolbar QToolButton:hover { background: @hover; color: @text; }
QToolBar#MainToolbar QToolButton:pressed { background: @pressed; }
QToolBar#MainToolbar QToolButton:checked { background: @accentSoft; color: @text; border-color: @accentEdge; }
QToolBar#MainToolbar QToolButton:disabled { color: @disabled; }
QToolBar#MainToolbar QToolButton[primary="true"] { background: @accent; color: @onAccent; border-color: @accent; }
QToolBar#MainToolbar QToolButton[primary="true"]:hover { background: @accentFill; border-color: @accentFill; }
QToolBar#MainToolbar QToolButton[primary="true"]:pressed { background: @accentPressed; }

QPushButton { background: @raised; color: @text; border: 1px solid @borderStrong; border-radius: 6px; padding: 0px 14px; min-height: 30px; }
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:focus { border-color: @accent; }
QPushButton:disabled { color: @disabled; background: @panel; border-color: @border; }
QPushButton[kind="primary"] { background: @accent; border-color: @accent; color: @onAccent; font-weight: 600; }
QPushButton[kind="primary"]:hover { background: @accentFill; border-color: @accentFill; }
QPushButton[kind="primary"]:pressed { background: @accentPressed; }
QPushButton[kind="primary"]:disabled { background: @hover; border-color: @hover; color: @disabled; }
QPushButton[kind="ghost"] { background: transparent; border-color: transparent; color: @text2; }
QPushButton[kind="ghost"]:hover { background: @hover; color: @text; }
QPushButton[kind="ghost"]:disabled { color: @disabled; background: transparent; }
QPushButton[kind="link"] { background: transparent; border: none; color: @accentHover; padding: 0px; min-height: 0px; text-align: left; }
QPushButton[kind="link"]:hover { color: @text; }
QPushButton[kind="link"]:disabled { color: @disabled; }
QPushButton[kind="row"] { background: transparent; border: 1px solid transparent; border-radius: 6px; text-align: left; padding: 6px 8px; min-height: 24px; }
QPushButton[kind="row"]:hover { background: @hover; }
QPushButton[kind="row"]:checked { background: @accentSoft; border-color: @accentEdge; }
QPushButton[kind="section"] { background: transparent; border: none; color: @muted; font-size: 9pt; font-weight: 600; letter-spacing: 0.6px; text-align: left; padding: 2px 0px; min-height: 22px; }
QPushButton[kind="section"]:hover { color: @text2; }
QPushButton[kind="section"]:focus { color: @text; }
QPushButton[kind="segment"] { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 0px 10px; min-height: 26px; color: @text2; }
QPushButton[kind="segment"]:hover { background: @hover; color: @text; }
QPushButton[kind="segment"]:checked { background: @pressed; color: @text; }

QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 4px; color: @text2; }
QToolButton:hover { background: @hover; color: @text; }
QToolButton:pressed { background: @pressed; }
QToolButton:checked { background: @accentSoft; color: @text; border-color: @accentEdge; }
QToolButton:disabled { color: @disabled; }
QToolButton::menu-indicator { image: none; width: 0px; }
QToolButton[kind="text"] { padding: 4px 10px; }

QFrame[overlay="true"] { background: @overlay; border: 1px solid @border; border-radius: 10px; }
QFrame[card="true"] { background: @raised; border: 1px solid @border; border-radius: 10px; }
QFrame[hint="true"] { background: @overlay; border: 1px solid @borderStrong; border-radius: 18px; }
QFrame[divider="true"] { background: @border; border: none; max-height: 1px; min-height: 1px; }
QFrame[vdivider="true"] { background: @border; border: none; max-width: 1px; min-width: 1px; }
QFrame[overlay="true"] QToolButton:checked { background: @hover; border-color: transparent; }
QFrame[overlay="true"] QToolButton:checked:hover { background: @pressed; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { background: @input; color: @text; border: 1px solid @borderStrong; border-radius: 6px; padding: 4px 8px; min-height: 22px; selection-background-color: @accent; selection-color: @onAccent; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: @accent; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { color: @disabled; border-color: @border; }
QComboBox QAbstractItemView { background: @raised; border: 1px solid @borderStrong; selection-background-color: @accentSoft; color: @text; outline: 0px; padding: 4px; }
QCheckBox, QRadioButton { color: @text; spacing: 8px; background: transparent; }
QCheckBox::indicator, QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid @borderStrong; background: @input; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover { border-color: @accent; }
QCheckBox::indicator:checked { background: @accent; border-color: @accent; image: url(@img/check.png); }
QRadioButton::indicator:checked { background: @accent; border-color: @accent; image: url(@img/dot.png); }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled { background: @panel; border-color: @border; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox::down-arrow { image: url(@img/down.png); width: 14px; height: 14px; }
QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { border: none; background: transparent; width: 18px; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(@img/up.png); width: 12px; height: 12px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(@img/down.png); width: 12px; height: 12px; }
QPushButton[kind="choice"] { background: @input; border: 1px solid @borderStrong; border-radius: 6px; padding: 0px 10px; min-height: 28px; color: @text2; }
QPushButton[kind="choice"]:hover { border-color: @accent; color: @text; }
QPushButton[kind="choice"]:checked { background: @accentSoft; border-color: @accent; color: @text; }
QCheckBox:disabled, QRadioButton:disabled { color: @disabled; }

QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px 2px 2px 0px; }
QScrollBar::handle:vertical { background: @borderStrong; border-radius: 3px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: @scrollHover; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0px 2px 2px 2px; }
QScrollBar::handle:horizontal { background: @borderStrong; border-radius: 3px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: @scrollHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0px; height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTableWidget, QTreeWidget, QListWidget { background: @input; color: @text2; border: 1px solid @border; border-radius: 6px; gridline-color: @border; outline: 0px; }
QTableWidget::item, QTreeWidget::item { padding: 2px 4px; }
QTableWidget::item:selected, QTreeWidget::item:selected { background: @accentSoft; color: @text; }
QHeaderView::section { background: @panel; color: @muted; border: none; border-bottom: 1px solid @border; padding: 4px 6px; }
QTableCornerButton::section { background: @panel; border: none; }
QPlainTextEdit { background: @input; color: @text2; border: 1px solid @border; border-radius: 6px; padding: 4px; selection-background-color: @accent; }

QStatusBar { background: @app; border-top: 1px solid @border; color: @text2; min-height: 30px; }
QStatusBar::item { border: none; }
QStatusBar QLabel { color: @text2; }
QProgressBar { background: @hover; border: none; border-radius: 3px; max-height: 6px; min-height: 6px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }
QSplitter::handle { background: transparent; }
QSplitter::handle:vertical { height: 8px; }
QSplitter::handle:horizontal { width: 8px; }
QDockWidget { color: @text; }

/* 3re studio shell */
QWidget[surface="rail"] { background: @app; border-right: 1px solid @border; }
QToolButton[kind="nav"] { background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 8px 4px 6px 4px; color: @text2; font-size: 9pt; }
QToolButton[kind="nav"]:hover { background: @hover; color: @text; }
QToolButton[kind="nav"]:checked { background: @accentSoft; border-color: @accentEdge; color: @text; font-weight: 600; }
QToolButton[kind="nav"]:focus:!checked { border-color: @border; }
QWidget[surface="header"] { background: @app; border-bottom: 1px solid @border; }
QPushButton[kind="step"] { background: transparent; border: 1px solid @border; border-radius: 16px; padding: 0px 14px; min-height: 30px; color: @text2; }
QPushButton[kind="step"][step="current"] { background: @accentSoft; border-color: @accentHover; color: @text; font-weight: 600; }
QPushButton[kind="step"][step="done"] { color: @text; border-color: @borderStrong; }
QPushButton[kind="step"][step="done"]:hover { background: @hover; }
QPushButton[kind="step"]:disabled { color: @disabled; border-color: @border; }
QLabel[stepLink="true"] { background: @borderStrong; }
QFrame[homeCard="true"] { background: @raised; border: 1px solid @border; border-radius: 14px; }
QFrame[homeCard="true"]:hover { border-color: @accentEdge; }
QLabel[type="hero"] { font-size: 26pt; font-weight: 600; }
)");
  const std::pair<const char*, QString> vars[] = {
      {"@accentPressed", css(c.accent_pressed)},
      {"@accentFill", css(c.accent_fill_hover)},
      {"@accentHover", css(c.accent_hover)},
      {"@accentSoft", css(with_alpha(c.accent, 0.18))},
      {"@accentEdge", css(with_alpha(c.accent, 0.45))},
      {"@accent", css(c.accent)},
      {"@onAccent", css(c.on_accent)},
      {"@app", css(c.app)},
      {"@panel", css(c.panel)},
      {"@raised", css(c.raised)},
      {"@hover", css(c.hover)},
      {"@pressed", css(c.pressed)},
      {"@input", css(c.input)},
      {"@overlay", css(with_alpha(c.panel, 0.88))},
      {"@borderStrong", css(c.border_strong)},
      {"@border", css(c.border)},
      {"@scrollHover", css(c.text_muted)},
      {"@text2", css(c.text_secondary)},
      {"@text", css(c.text)},
      {"@muted", css(c.text_muted)},
      {"@disabled", css(c.text_disabled)},
      {"@passSoft", css(with_alpha(c.pass, 0.16))},
      {"@reviewSoft", css(with_alpha(c.review, 0.16))},
      {"@failSoft", css(with_alpha(c.fail, 0.16))},
      {"@infoSoft", css(with_alpha(c.info, 0.16))},
      {"@img", ui_images_dir()},
      {"@pass", css(c.pass)},
      {"@review", css(c.review)},
      {"@fail", css(c.fail)},
      {"@info", css(c.info)},
  };
  // Longer names first in the table, so "@accentHover" is never cut by "@accent".
  for (const auto& [k, v] : vars) s.replace(QLatin1String(k), v);
  return s;
}

void apply(QApplication& app) {
  app.setStyle(QStyleFactory::create("Fusion"));
  const Colors& c = colors();
  QPalette p;
  p.setColor(QPalette::Window, c.panel);
  p.setColor(QPalette::WindowText, c.text);
  p.setColor(QPalette::Base, c.input);
  p.setColor(QPalette::AlternateBase, c.raised);
  p.setColor(QPalette::ToolTipBase, c.raised);
  p.setColor(QPalette::ToolTipText, c.text);
  p.setColor(QPalette::PlaceholderText, c.text_muted);
  p.setColor(QPalette::Text, c.text);
  p.setColor(QPalette::Button, c.raised);
  p.setColor(QPalette::ButtonText, c.text);
  p.setColor(QPalette::BrightText, c.on_accent);
  p.setColor(QPalette::Light, c.hover);
  p.setColor(QPalette::Midlight, c.raised);
  p.setColor(QPalette::Mid, c.border_strong);
  p.setColor(QPalette::Dark, c.app);
  p.setColor(QPalette::Shadow, Qt::black);
  p.setColor(QPalette::Highlight, c.accent);
  p.setColor(QPalette::HighlightedText, c.on_accent);
  p.setColor(QPalette::Link, c.accent_hover);
  for (QPalette::ColorRole r : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
    p.setColor(QPalette::Disabled, r, c.text_disabled);
  app.setPalette(p);
  app.setFont(font(Type::Body));
  app.setStyleSheet(stylesheet());
}

void repolish(QWidget* w) {
  if (!w) return;
  w->style()->unpolish(w);
  w->style()->polish(w);
  w->update();
}

void set_prop(QWidget* w, const char* name, const QVariant& value) {
  if (!w || w->property(name) == value) return;
  w->setProperty(name, value);
  repolish(w);
}

}  // namespace p3dgui::theme
