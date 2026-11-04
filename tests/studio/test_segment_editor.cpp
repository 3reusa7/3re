#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QPointer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "registry.hpp"
#include "segmentation/mask_set.hpp"
#include "segmentation/segment_editor.hpp"
#include "studio/image3d_workspace.hpp"

namespace {
QImage rect_mask(QSize s, QRect r) {
  QImage m(s, QImage::Format_Grayscale8);
  m.fill(0);
  for (int y = r.top(); y <= r.bottom(); ++y)
    for (int x = r.left(); x <= r.right(); ++x) m.scanLine(y)[x] = 255;
  return m;
}
QString subject_png(const QTemporaryDir& dir) {
  QImage img(120, 80, QImage::Format_ARGB32);
  img.fill(QColor(230, 230, 230));
  for (int y = 20; y < 60; ++y)
    for (int x = 20; x < 50; ++x) img.setPixelColor(x, y, QColor(200, 40, 40));
  for (int y = 20; y < 60; ++y)
    for (int x = 70; x < 100; ++x) img.setPixelColor(x, y, QColor(40, 40, 200));
  const QString f = dir.filePath("subject.png");
  img.save(f);
  return f;
}
}  // namespace

class SegmentEditorTests : public QObject {
  Q_OBJECT
 private slots:
  void editor_undo_redo_via_shortcuts() {
    p3dgui::SegmentEditor ed;
    ed.setEngineEnabled(false);
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::gray);
    ed.setImage(img, QString(64, 'a'));
    ed.show();
    QVERIFY(QTest::qWaitForWindowExposed(&ed));
    ed.masks()->add(rect_mask(ed.masks()->size(), QRect(0, 0, 8, 8)), "Body");
    QCOMPARE(ed.masks()->regions().size(), 1);
    QTest::keyClick(&ed, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(ed.masks()->regions().size(), 0);
    QTest::keyClick(&ed, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(ed.masks()->regions().size(), 1);
    QTest::keyClick(&ed, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(&ed, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(ed.masks()->regions().size(), 1);
  }
  void brush_stroke_keeps_the_region_rows() {
    p3dgui::SegmentEditor ed;
    ed.setEngineEnabled(false);
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::gray);
    ed.setImage(img, QString(64, 'a'));
    const int id = ed.masks()->add(rect_mask(ed.masks()->size(), QRect(0, 0, 8, 8)), "Body");
    ed.setCurrentRegion(id);
    QWidget* row = ed.findChild<QWidget*>("segRow");
    QVERIFY(row);
    QPointer<QWidget> guard(row);
    ed.masks()->beginStroke(id);
    for (int x = 10; x < 40; x += 2) ed.masks()->paint(id, QPoint(x, 20), 3, false);
    ed.masks()->endStroke();
    QCoreApplication::processEvents();
    QVERIFY2(guard, "painting must not rebuild the region list");
  }
  void custom_label_commits_once() {
    p3dgui::SegmentEditor ed;
    ed.setEngineEnabled(false);
    QImage img(64, 48, QImage::Format_RGB32);
    img.fill(Qt::gray);
    ed.setImage(img, QString(64, 'a'));
    const int id = ed.masks()->add(rect_mask(ed.masks()->size(), QRect(0, 0, 8, 8)), "Body");
    auto* combo = ed.findChild<QComboBox*>("segLabel");
    QVERIFY(combo && combo->lineEdit());
    const int steps = ed.masks()->undo().count();
    combo->lineEdit()->clear();
    QTest::keyClicks(combo->lineEdit(), "Cape");
    QCoreApplication::processEvents();
    QCOMPARE(ed.masks()->undo().count(), steps);  // nothing committed while typing
    QCOMPARE(combo, ed.findChild<QComboBox*>("segLabel"));
    emit combo->lineEdit()->editingFinished();
    QCoreApplication::processEvents();
    QCOMPARE(ed.masks()->region(id)->label, QString("Cape"));
    QCOMPARE(ed.masks()->undo().count(), steps + 1);
  }
  void colour_click_adds_region_without_sam() {
    QTemporaryDir dir;
    p3dgui::SegmentEditor ed;
    ed.setEngineEnabled(false);
    ed.setImage(QImage(subject_png(dir)), QString(64, 'a'));
    ed.clickAt(QPointF(30, 30), p3dgui::SegmentEditor::ClickMode::NewRegion);
    QTRY_COMPARE_WITH_TIMEOUT(ed.masks()->regions().size(), 1, 3000);
    const auto& m = ed.masks()->regions().front().mask;
    QVERIFY(m.constScanLine(30)[30] > 127);
    QVERIFY(m.constScanLine(30)[80] < 128);
    QVERIFY(ed.statusText().contains("Colour", Qt::CaseInsensitive));
  }
  void editor_autosaves_to_project() {
    QTemporaryDir dir;
    const QString file = subject_png(dir);
    p3dgui::Image3DWorkspace ws(true);
    ws.setProjectsDir(dir.filePath("Projects"));
    ws.segmentEditor()->setEngineEnabled(false);
    ws.importImage(file);
    QTRY_VERIFY_WITH_TIMEOUT(ws.statusText().startsWith("Cutout ready"), 5000);
    auto* masks = ws.segmentEditor()->masks();
    QVERIFY(masks);
    masks->add(rect_mask(masks->size(), QRect(20, 20, 30, 40)), "Body");
    QTRY_VERIFY_WITH_TIMEOUT(ws.project().has_value(), 3000);
    const QString json = ws.project()->segmentationDir() + "/segmentation.json";
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(json).isFile(), 3000);
    const QString projectFile = ws.project()->projectFile();

    p3dgui::Image3DWorkspace again(true);
    again.segmentEditor()->setEngineEnabled(false);
    again.openProject(projectFile);
    QTRY_VERIFY_WITH_TIMEOUT(again.segmentEditor()->masks() && again.segmentEditor()->masks()->regions().size() == 1, 5000);
    QCOMPARE(again.segmentEditor()->masks()->regions().front().label, QString("Body"));
  }
  void generate_uses_included_regions() {
    QTemporaryDir dir;
    p3dgui::Image3DWorkspace ws(true);
    ws.setProjectsDir(dir.filePath("Projects"));
    ws.segmentEditor()->setEngineEnabled(false);
    ws.importImage(subject_png(dir));
    QTRY_VERIFY_WITH_TIMEOUT(ws.statusText().startsWith("Cutout ready"), 5000);
    auto* masks = ws.segmentEditor()->masks();
    const int red = masks->add(rect_mask(masks->size(), QRect(20, 20, 30, 40)), "Body");
    const int blue = masks->add(rect_mask(masks->size(), QRect(70, 20, 30, 40)), "Object");
    masks->setInclude(blue, false);
    const QImage cut = ws.generationCutout();
    QCOMPARE(cut.size(), QSize(120, 80));
    QCOMPARE(qAlpha(cut.pixel(30, 30)), 255);  // included region
    QCOMPARE(qAlpha(cut.pixel(80, 30)), 0);    // excluded region
    QCOMPARE(qAlpha(cut.pixel(5, 5)), 0);      // not marked
    masks->remove(red);
    masks->remove(blue);
    QVERIFY(ws.generationCutout().cacheKey() != cut.cacheKey());  // no regions: the automatic cutout
  }
};
THREERE_TEST(SegmentEditorTests)
#include "test_segment_editor.moc"
