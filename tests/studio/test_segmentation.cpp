#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "registry.hpp"
#include "segmentation/color_regions.hpp"
#include "segmentation/mask_set.hpp"
#include "segmentation/segmentation_store.hpp"

using namespace p3dseg;

namespace {
QImage rect_mask(QSize s, QRect r) {
  QImage m(s, QImage::Format_Grayscale8);
  m.fill(0);
  for (int y = r.top(); y <= r.bottom(); ++y)
    for (int x = r.left(); x <= r.right(); ++x) m.scanLine(y)[x] = 255;
  return m;
}
int area(const QImage& m) {
  int n = 0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) n += m.constScanLine(y)[x] > 127;
  return n;
}
}  // namespace

class SegmentationTests : public QObject {
  Q_OBJECT
 private slots:
  void undo_redo_every_op() {
    MaskSet set(QSize(40, 30));
    const int a = set.add(rect_mask(set.size(), QRect(0, 0, 10, 10)), "Body");
    const int b = set.add(rect_mask(set.size(), QRect(20, 0, 10, 10)), "Weapon");
    QCOMPARE(set.regions().size(), 2);
    set.setLabel(b, "Armor");
    set.setName(b, "Shield");
    set.setInclude(b, false);
    QCOMPARE(set.region(b)->label, QString("Armor"));
    QVERIFY(!set.region(b)->include);
    set.merge({a, b});
    QCOMPARE(set.regions().size(), 1);
    QCOMPARE(area(set.region(a)->mask), 200);
    set.remove(a);
    QVERIFY(set.regions().isEmpty());
    // Every step back, in order.
    set.undo().undo();
    QCOMPARE(set.regions().size(), 1);
    set.undo().undo();
    QCOMPARE(set.regions().size(), 2);
    QVERIFY(!set.region(b)->include);
    set.undo().undo();
    QVERIFY(set.region(b)->include);
    set.undo().undo();
    QCOMPARE(set.region(b)->name, QString("Weapon"));  // a new region is named after its first label
    set.undo().undo();
    QCOMPARE(set.region(b)->label, QString("Weapon"));
    set.undo().undo();
    set.undo().undo();
    QVERIFY(set.regions().isEmpty());
    for (int i = 0; i < 7; ++i) set.undo().redo();
    QVERIFY(set.regions().isEmpty());
  }
  void stroke_is_one_undo_step() {
    MaskSet set(QSize(64, 64));
    const int id = set.add(rect_mask(set.size(), QRect(0, 0, 8, 8)), "Hair");
    const int before = set.undo().count();
    set.beginStroke(id);
    for (int x = 20; x < 40; x += 2) set.paint(id, QPoint(x, 30), 3, false);
    set.endStroke();
    QCOMPARE(set.undo().count(), before + 1);
    QVERIFY(area(set.region(id)->mask) > 64);
    set.undo().undo();
    QCOMPARE(area(set.region(id)->mask), 64);
    set.beginStroke(id);
    set.paint(id, QPoint(2, 2), 2, true);
    set.endStroke();
    QVERIFY(area(set.region(id)->mask) < 64);
  }
  void merge_then_split_restores_components() {
    MaskSet set(QSize(50, 20));
    const int a = set.add(rect_mask(set.size(), QRect(0, 0, 10, 10)), "Body");
    const int b = set.add(rect_mask(set.size(), QRect(30, 5, 10, 10)), "Body");
    set.merge({a, b});
    QCOMPARE(set.regions().size(), 1);
    const auto parts = set.split(a);
    QCOMPARE(parts.size(), 1);  // one new region; the first component keeps id a
    QCOMPARE(set.regions().size(), 2);
    QCOMPARE(area(set.region(a)->mask), 100);
    QCOMPARE(area(set.region(parts[0])->mask), 100);
    QVERIFY(set.split(a).isEmpty());  // a single component does not split
  }
  void union_respects_include() {
    MaskSet set(QSize(20, 10));
    const int a = set.add(rect_mask(set.size(), QRect(0, 0, 5, 5)), "Body");
    const int b = set.add(rect_mask(set.size(), QRect(10, 0, 5, 5)), "Background");
    set.setInclude(b, false);
    QCOMPARE(area(set.unionMask(true)), 25);
    QCOMPARE(area(set.unionMask(false)), 50);
    Q_UNUSED(a);
  }
  void store_roundtrip() {
    QTemporaryDir dir;
    MaskSet set(QSize(32, 16));
    const int a = set.add(rect_mask(set.size(), QRect(1, 1, 6, 6)), "Head");
    set.setName(a, "Knight head");
    const int b = set.add(rect_mask(set.size(), QRect(10, 2, 4, 9)), "Weapon");
    set.setInclude(b, false);
    SegmentationStore::save(dir.path(), set, QString(64, 'a'), QSize(64, 32));
    QVERIFY(QFileInfo(dir.filePath("segmentation.json")).isFile());
    QString error;
    auto back = SegmentationStore::load(dir.path(), QString(64, 'a'), &error);
    QVERIFY2(back, qPrintable(error));
    QCOMPARE(back->size(), QSize(32, 16));
    QCOMPARE(back->regions().size(), 2);
    QCOMPARE(back->region(a)->name, QString("Knight head"));
    QCOMPARE(back->region(a)->label, QString("Head"));
    QVERIFY(!back->region(b)->include);
    QCOMPARE(area(back->region(b)->mask), 36);
    QCOMPARE(back->undo().count(), 0);  // a loaded set starts a fresh history
  }
  void store_refuses_changed_image() {
    QTemporaryDir dir;
    MaskSet set(QSize(8, 8));
    set.add(rect_mask(set.size(), QRect(0, 0, 4, 4)), "Body");
    SegmentationStore::save(dir.path(), set, QString(64, 'a'), QSize(8, 8));
    QString error;
    QVERIFY(!SegmentationStore::load(dir.path(), QString(64, 'b'), &error));
    QVERIFY2(error.contains("changed"), qPrintable(error));
    QVERIFY(QFileInfo(dir.filePath("segmentation.json")).isFile());  // kept on disk
  }
  void store_missing_is_not_an_error() {
    QTemporaryDir dir;
    QString error;
    QVERIFY(!SegmentationStore::load(dir.path(), QString(64, 'a'), &error));
    QVERIFY(error.isEmpty());
  }
  void large_image_bounded() {
    QCOMPARE(working_size(QSize(6000, 4000)), QSize(2048, 1365));
    QCOMPARE(working_size(QSize(800, 600)), QSize(800, 600));
    QCOMPARE(working_size(QSize(1000, 5000)), QSize(410, 2048));
  }
  void color_region_flood() {
    QImage img(30, 10, QImage::Format_ARGB32);
    img.fill(Qt::white);
    for (int y = 2; y < 8; ++y)
      for (int x = 2; x < 12; ++x) img.setPixelColor(x, y, QColor(200, 30, 30));
    for (int y = 2; y < 8; ++y)
      for (int x = 15; x < 25; ++x) img.setPixelColor(x, y, QColor(30, 30, 200));
    const QImage red = color_region_at(img, QPoint(5, 5), 24);
    QCOMPARE(area(red), 60);
    const auto all = suggest_color_regions(img, 20);
    QVERIFY(all.size() >= 2);
    QCOMPARE(area(all[0]), 60);  // the two coloured blocks; the white border background is left out
  }
  void clean_mask_fills_enclosed_holes_and_drops_specks() {
    QImage m = rect_mask(QSize(100, 100), QRect(10, 10, 60, 60));
    for (int y = 30; y < 36; ++y)
      for (int x = 30; x < 36; ++x) m.scanLine(y)[x] = 0;  // enclosed hole (36 px)
    for (int y = 10; y < 20; ++y) m.scanLine(y)[40] = 0;   // notch open to the outside: kept
    for (int y = 0; y < 2; ++y)
      for (int x = 90; x < 92; ++x) m.scanLine(y)[x] = 255;  // 4-pixel speck far away
    const QImage c = clean_mask(m);
    QCOMPARE(int(c.constScanLine(32)[32]), 255);
    QCOMPARE(int(c.constScanLine(12)[40]), 0);
    QCOMPARE(int(c.constScanLine(0)[90]), 0);
    QCOMPARE(area(c), 3600 - 10);
  }
  void cutout_alpha_matches_union() {
    QImage original(40, 20, QImage::Format_ARGB32);
    original.fill(QColor(10, 200, 10));
    const QImage mask = rect_mask(QSize(20, 10), QRect(0, 0, 10, 10));  // left half at half resolution
    const QImage cut = apply_cutout(original, mask);
    QCOMPARE(cut.size(), original.size());
    QCOMPARE(qAlpha(cut.pixel(5, 10)), 255);
    QCOMPARE(qAlpha(cut.pixel(35, 10)), 0);
    QCOMPARE(QColor(cut.pixel(5, 10)).green(), 200);
  }
};
THREERE_TEST(SegmentationTests)
#include "test_segmentation.moc"
