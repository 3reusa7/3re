#include <QAbstractButton>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <cmath>

#include "registry.hpp"
#include "shell/coming_soon.hpp"
#include "shell/feature_badge.hpp"
#include "shell/nav_rail.hpp"
#include "shell/spinner.hpp"
#include "shell/step_bar.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {
double luminance(const QColor& c) {
  auto ch = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * ch(c.red()) + 0.7152 * ch(c.green()) + 0.0722 * ch(c.blue());
}
double contrast(const QColor& a, const QColor& b) {
  const double la = luminance(a), lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}
}  // namespace

class ShellWidgetTests : public QObject {
  Q_OBJECT
 private slots:
  void accent_contrast_ok() {
    const auto& c = p3dgui::theme::colors();
    QCOMPARE(c.accent.name(), QString("#5b72a6"));
    for (const QColor& surface : {c.app, c.panel, c.raised}) {
      QVERIFY2(contrast(c.text, surface) >= 4.5, "body text");
      QVERIFY2(contrast(c.text_secondary, surface) >= 4.5, "secondary text");
      QVERIFY2(contrast(c.text_muted, surface) >= 4.5, "muted text");
      QVERIFY2(contrast(c.accent_hover, surface) >= 4.5, "link / accent text");
    }
    QVERIFY2(contrast(c.on_accent, c.accent) >= 4.5, "primary button label");
  }
  void nav_rail_emits_selected() {
    p3dgui::NavRail rail;
    rail.addItem({}, "Home", "home");
    rail.addItem({}, "Export", "export");
    QSignalSpy spy(&rail, &p3dgui::NavRail::selected);
    rail.setCurrent("export");
    QCOMPARE(rail.current(), QString("export"));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QString("export"));
    rail.setCurrent("export");
    QCOMPARE(spy.count(), 1);  // no repeat for the current page
  }
  void step_bar_only_reachable_steps_clickable() {
    p3dgui::StepBar bar({"Import", "Segment", "Generate", "Preview"});
    bar.setCurrent(1);
    bar.setCompleted(0, true);
    QSignalSpy spy(&bar, &p3dgui::StepBar::stepClicked);
    const auto buttons = bar.findChildren<QAbstractButton*>();
    QCOMPARE(buttons.size(), 4);
    QVERIFY(buttons[0]->isEnabled());
    QVERIFY(buttons[1]->isEnabled());
    QVERIFY(!buttons[2]->isEnabled());
    QVERIFY(!buttons[3]->isEnabled());
    buttons[0]->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 0);
  }
  void coming_soon_has_no_buttons() {
    p3dgui::ComingSoonPanel panel("Animation", "Keyframe and clip animation for your models.", {"A rigged model"});
    QVERIFY(panel.findChildren<QAbstractButton*>().isEmpty());
    bool badge = false;
    for (auto* l : panel.findChildren<QLabel*>()) badge |= l->text() == "Coming Soon";
    QVERIFY(badge);
  }
  void feature_badges_have_text() {
    std::unique_ptr<QLabel> e(p3dgui::make_feature_badge(p3dgui::Feature::Experimental));
    std::unique_ptr<QLabel> s(p3dgui::make_feature_badge(p3dgui::Feature::ComingSoon));
    QCOMPARE(e->text(), QString("Experimental"));
    QCOMPARE(s->text(), QString("Coming Soon"));
  }
  void studio_icons_paint() {
    using p3dgui::Icon;
    for (Icon id : {Icon::Home, Icon::Retopo, Icon::Bone, Icon::Animation, Icon::Regions, Icon::Eraser, Icon::BoxSelect,
                    Icon::ClickSelect, Icon::Folder, Icon::ImageTo3D}) {
      const QImage img = p3dgui::icon(id).pixmap(48, 48).toImage();
      int ink = 0;
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) ink += qAlpha(img.pixel(x, y)) > 40;
      QVERIFY2(ink > 60, qPrintable(QString("icon %1 paints %2 px").arg(int(id)).arg(ink)));
    }
  }
  void spinner_runs_and_stops() {
    p3dgui::Spinner s;
    QVERIFY(!s.running());
    s.start();
    QVERIFY(s.running());
    s.stop();
    QVERIFY(!s.running());
  }
};
THREERE_TEST(ShellWidgetTests)
#include "test_shell_widgets.moc"
