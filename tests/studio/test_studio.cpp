#include <QAbstractButton>
#include <QPushButton>
#include <QTemporaryDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QTest>
#include <QUrl>

#include "main_window.hpp"
#include "registry.hpp"
#include "shell/coming_soon.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/retopo_workspace.hpp"
#include "studio/studio_window.hpp"

namespace {
p3dgui::WindowOptions options() {
  p3dgui::WindowOptions o;
  o.software_view = true;
  return o;
}
int enabled_buttons(QWidget* w) {
  int n = 0;
  for (auto* b : w->findChildren<QAbstractButton*>())
    if (b->isEnabled()) ++n;
  return n;
}
}  // namespace

class StudioTests : public QObject {
  Q_OBJECT
 private slots:
  void studio_has_eight_pages() {
    p3dgui::StudioWindow s(options());
    // Milestone 4 (W1): the Agent is the first workspace and the start page.
    const QStringList ids{"agent", "home", "image", "retopo", "export", "rigging", "animation", "settings"};
    QCOMPARE(s.pageIds(), ids);
    QCOMPARE(s.currentPage(), QString("agent"));
    for (const QString& id : ids) {
      s.showPage(id);
      QCOMPARE(s.currentPage(), id);
      QVERIFY(s.page(id));
    }
    QCOMPARE(s.windowTitle(), QString("3re"));
  }
  void ctrl_numbers_switch_pages() {
    p3dgui::StudioWindow s(options());
    s.show();
    QVERIFY(QTest::qWaitForWindowExposed(&s));
    // Shortcuts follow the rail: Ctrl+1 Agent, Ctrl+2 Home, ..., Ctrl+6 Rigging.
    QTest::keyClick(&s, Qt::Key_6, Qt::ControlModifier);
    QCOMPARE(s.currentPage(), QString("rigging"));
    QTest::keyClick(&s, Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(s.currentPage(), QString("home"));
    QTest::keyClick(&s, Qt::Key_1, Qt::ControlModifier);
    QCOMPARE(s.currentPage(), QString("agent"));
  }
  void animation_is_coming_soon() {
    p3dgui::StudioWindow s(options());
    QVERIFY(qobject_cast<p3dgui::ComingSoonPanel*>(s.page("animation")) || s.page("animation")->findChild<p3dgui::ComingSoonPanel*>());
    QCOMPARE(enabled_buttons(s.page("animation")), 0);
  }
  void retopology_is_a_workspace_waiting_for_a_model() {
    p3dgui::StudioWindow s(options());
    auto* retopo = qobject_cast<p3dgui::RetopoWorkspace*>(s.page("retopo"));
    QVERIFY(retopo);
    QVERIFY(!s.page("retopo")->findChild<p3dgui::ComingSoonPanel*>("ComingSoonPanel"));
    // Nothing to run or save before a model is chosen; no generated model yet either.
    QVERIFY(!s.page("retopo")->findChild<QPushButton*>("retopoRun")->isEnabled());
    QVERIFY(!s.page("retopo")->findChild<QPushButton*>("retopoSave")->isEnabled());
    QVERIFY(!s.page("retopo")->findChild<QPushButton*>("retopoUseGenerated")->isEnabled());
    QVERIFY(s.page("retopo")->findChild<QPushButton*>("retopoOpen")->isEnabled());
  }
  void rigging_coming_soon_without_lowpoly_project() {
    p3dgui::StudioWindow s(options());
    s.show();
    s.showPage("rigging");
    QTest::qWait(50);
    auto* soon = s.page("rigging")->findChild<p3dgui::ComingSoonPanel*>();
    QVERIFY(soon);
    QVERIFY(soon->isVisible());
  }
  void pixel_builder_is_existing_main_window() {
    p3dgui::StudioWindow s(options());
    s.show();
    s.showPage("image");
    s.setImageMode("pixel");
    QTest::qWait(50);
    QCOMPARE(s.imageMode(), QString("pixel"));
    QVERIFY(s.pixelBuilder().isVisible());
    QVERIFY(s.isAncestorOf(&s.pixelBuilder()));
  }
  void home_start_cards_open_workflows() {
    p3dgui::StudioWindow s(options());
    auto* image = s.page("home")->findChild<QPushButton*>("homeStartImage");
    auto* pixel = s.page("home")->findChild<QPushButton*>("homeStartPixel");
    QVERIFY(image && pixel);
    image->click();
    QCOMPARE(s.currentPage(), QString("image"));
    QCOMPARE(s.imageMode(), QString("ai"));
    s.showPage("home");
    pixel->click();
    QCOMPARE(s.currentPage(), QString("image"));
    QCOMPARE(s.imageMode(), QString("pixel"));
  }
  void pixel_start_screen_trellis_button_opens_ai_page() {
    p3dgui::StudioWindow s(options());
    s.showPage("home");
    s.pixelBuilder().runAction("image3d");
    QCOMPARE(s.currentPage(), QString("image"));
    QCOMPARE(s.imageMode(), QString("ai"));
  }
  void workspace_accepts_dropped_image() {
    QTemporaryDir dir;
    QImage img(32, 32, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    for (int y = 8; y < 24; ++y)
      for (int x = 8; x < 24; ++x) img.setPixelColor(x, y, Qt::green);
    const QString file = dir.filePath("dropped.png");
    QVERIFY(img.save(file));
    p3dgui::Image3DWorkspace ws(true);
    ws.setProjectsDir(dir.filePath("Projects"));
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(file)});
    QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&ws, &enter);
    QVERIFY(enter.isAccepted());
    QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&ws, &drop);
    QTRY_VERIFY_WITH_TIMEOUT(ws.statusText().startsWith("Cutout ready"), 5000);
    QVERIFY(!QFileInfo::exists(dir.filePath("Projects")));  // looking at a picture writes nothing
  }
};
THREERE_TEST(StudioTests)
#include "test_studio.moc"
