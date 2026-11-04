#pragma once
// Pixel3D design system: every colour, font, spacing, radius, icon size and
// transition time of the desktop application lives here. Widgets choose a
// role (a Qt dynamic property such as type="title" or kind="primary") and the
// global stylesheet generated from these tokens styles it; painted content
// (viewport, previews, icons) reads the same tokens. Nothing else hard-codes
// a colour or a font size.
#include <QColor>
#include <QFont>
#include <QString>
#include <QVariant>

#include "pixel3d/engine/op_result.hpp"

class QApplication;
class QWidget;

namespace p3dgui::theme {

// Semantic colour language: neutral = normal, accent = selected/active,
// pass = validated, review = needs review, fail = blocking failure,
// info = estimated geometry / diagnostics only.
enum class Tone { Neutral, Accent, Pass, Review, Fail, Info };

struct Colors {
  // Surfaces, darkest to lightest.
  QColor app, panel, raised, hover, pressed, input;
  QColor viewport_top, viewport_bottom, preview_bg, checker;
  QColor border, border_strong;
  QColor text, text_secondary, text_muted, text_disabled, on_accent;
  QColor accent, accent_hover, accent_pressed;
  QColor accent_fill_hover;  // hovered accent-filled button (keeps white labels >= 4.5:1)
  QColor pass, review, fail, info, neutral;
  QColor axis_x, axis_y, axis_z;
  QColor icon;         // default icon stroke (a touch brighter than secondary text)
  QColor hover_tint;   // mixed into hovered geometry
  QColor pixel_grid;   // grid over zoomed drawings
  // The engine's comparison image (session_render.cpp), for the legend.
  QColor compare_match, compare_extra, compare_missing, compare_colour;
};
const Colors& colors();

QColor tone_color(Tone t);
const char* tone_name(Tone t);  // property value: "neutral", "accent", "pass", "review", "fail", "info"
Tone tone_for(pixel3d::ValidationStatus s);
QColor with_alpha(QColor c, double alpha);

namespace space {
constexpr int xs = 4, sm = 8, md = 12, lg = 16, xl = 24, xxl = 32;
}
namespace radius {
constexpr int sm = 4, md = 6, lg = 10;
}
namespace icon_size {
constexpr int sm = 16, md = 20, lg = 24, xl = 32;
}
namespace motion {
constexpr int fast = 120, normal = 160;  // ms
}
namespace metric {
constexpr int control = 32;         // button / input height
constexpr int row = 36;             // list row height
constexpr int toolbar_min = 64;     // toolbar button minimum width
constexpr int left_dock = 290, right_dock = 340, dock_min_left = 260, dock_min_right = 300;
constexpr int compact_width = 1280;  // below: icon-only toolbar
}

enum class Type { Small, Body, Section, Title, Status, Display, Mono };
QFont font(Type t);

// Installs Fusion, the palette, the application font and the stylesheet.
void apply(QApplication& app);
QString stylesheet();
// Sets a style property and re-applies the stylesheet to the widget.
void set_prop(QWidget* w, const char* name, const QVariant& value);
void repolish(QWidget* w);

}  // namespace p3dgui::theme
