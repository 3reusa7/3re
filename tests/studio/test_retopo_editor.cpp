#include <QComboBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QLabel>
#include <QtTest>
#include "registry.hpp"
#include "studio/retopo_mesh_editor.hpp"
#include "studio/retopo_workspace.hpp"
#include "theme.hpp"
#include "widgets.hpp"
#include "viewport.hpp"

class RetopoEditorTests : public QObject {
  Q_OBJECT
 private slots:
  // After Save GLB the saved, textured mesh is a third view; any edit removes it (the file no longer matches).
  void saved_texture_view_until_edited() {
    p3dgui::RetopoMeshEditor editor(true);
    editor.resize(900, 700);
    editor.setMesh({{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {{0,1,2,3}});
    editor.show();
    auto* views = editor.findChild<QComboBox*>("retopoComparison");
    QCOMPARE(views->count(), 2);
    auto saved = std::make_shared<pixel3d::TexturedMesh>();
    saved->positions = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
    saved->normals.assign(4, {0,0,1});
    saved->uvs = {{0,0},{1,0},{1,1},{0,1}};
    saved->indices = {0,1,2, 0,2,3};
    saved->texture = std::make_shared<pixel3d::ImageRGBA>(4, 4, pixel3d::Rgba8{200, 40, 40, 255});
    editor.setSaved(saved);
    QCOMPARE(views->count(), 3);
    QVERIFY(editor.showingSaved());
    QVERIFY(editor.viewport().scene().textured);
    QVERIFY(editor.viewport().scene().wire_edges && editor.viewport().scene().wire_edges->size() == 4 * 6);
    QVERIFY(!editor.findChild<QComboBox*>("retopoTransform")->isEnabled());
    views->setCurrentIndex(0);
    QVERIFY(!editor.viewport().scene().textured);
    editor.findChild<QComboBox*>("retopoSelection")->setCurrentIndex(2);
    QVERIFY(editor.findChild<QPushButton*>("retopoRevert")->isEnabled());
    QTest::mouseClick(editor.findChild<QPushButton*>("retopoRevert"), Qt::LeftButton);  // an edit
    QCOMPARE(views->count(), 2);
    QVERIFY(!editor.showingSaved());
  }
  void sidebar_fits_without_horizontal_clipping_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("minimum-window") << QSize(1012, 636);
    QTest::newRow("laptop") << QSize(1278, 704);
    QTest::newRow("desktop") << QSize(1352, 836);
  }
  void sidebar_fits_without_horizontal_clipping() {
    QFETCH(QSize, size);
    p3dgui::RetopoWorkspace workspace(true);
    workspace.setStyleSheet(p3dgui::theme::stylesheet());
    workspace.resize(size);
    workspace.setAdvanced(true);
    workspace.findChild<p3dgui::Section*>("retopoAdvanced")->setExpanded(true, false);
    workspace.findChild<QLabel*>("retopoSourceName")->setText(QString(180, 'M') + ".glb");
    workspace.show();
    QTest::qWait(100);
    auto* scroll = workspace.findChild<QScrollArea*>("retopoOptionsScroll");
    QVERIFY(scroll);
    if (scroll->widget()->width() > scroll->viewport()->width() || workspace.size() != size) {
      qWarning() << "layout" << workspace.size() << "sidebar" << scroll->widget()->size() << "viewport" << scroll->viewport()->size();
      for (auto* child : workspace.findChildren<QWidget*>())
        if (child->isVisible() && child->minimumSizeHint().width() > 250)
          qWarning() << child->metaObject()->className() << child->objectName() << child->size() << child->minimumSizeHint();
    }
    QCOMPARE(workspace.size(), size);
    QVERIFY(scroll->widget()->width() <= scroll->viewport()->width());
    QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
    // Hiding a scrollbar alone must not pass: every control must fit the content.
    for (auto* button : scroll->widget()->findChildren<QPushButton*>()) {
      if (!button->isVisible()) continue;
      const QRect bounds(button->mapTo(scroll->widget(), QPoint()), button->size());
      QVERIFY2(bounds.left() >= 0 && bounds.right() < scroll->viewport()->width(), qPrintable(button->text()));
      QVERIFY2(button->width() >= button->minimumSizeHint().width(), qPrintable(button->text()));
      scroll->ensureWidgetVisible(button);
      QCoreApplication::processEvents();
      QVERIFY(scroll->viewport()->rect().contains(QRect(button->mapTo(scroll->viewport(), QPoint()), button->size())));
    }
    for (auto* combo : scroll->widget()->findChildren<QComboBox*>()) {
      if (!combo->isVisible()) continue;
      QVERIFY(combo->width() >= combo->minimumSizeHint().width());
      QVERIFY(combo->mapTo(scroll->widget(), QPoint(combo->width(), 0)).x() <= scroll->viewport()->width());
    }
  }
  void mouse_transform_data() {
    QTest::addColumn<int>("selection"); QTest::addColumn<int>("tool"); QTest::addColumn<int>("changed");
    QTest::newRow("vertex-move") << 0 << 1 << 1;
    QTest::newRow("edge-move") << 1 << 1 << 2;
    QTest::newRow("face-rotate") << 2 << 2 << 4;
    QTest::newRow("face-scale") << 2 << 3 << 4;
  }
  void mouse_transform() {
    QFETCH(int,selection); QFETCH(int,tool); QFETCH(int,changed);
    p3dgui::RetopoMeshEditor editor(true); editor.resize(900,700);
    editor.setMesh({{0,0,0},{1,0,0},{1,1,0},{0,1,0}},{{0,1,2,3}});
    editor.show(); editor.viewport().setPreset(pixel3d::ViewRole::Front); QTest::qWait(150);
    editor.findChild<QComboBox*>("retopoSelection")->setCurrentIndex(selection);
    const auto& v=editor.viewport().scene().solid->positions;
    const auto p=editor.viewport().project((v[0]+v[1]+v[2]+v[3])*.25).toPoint();
    auto* surface=editor.viewport().surface();
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,p);
    editor.findChild<QComboBox*>("retopoTransform")->setCurrentIndex(tool);
    const auto before=editor.document().positions();
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,p);
    QTest::mouseMove(surface,p+QPoint(30,0),30);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,p+QPoint(30,0));
    int count=0; for(size_t i=0;i<before.size();++i) if(before[i]!=editor.document().positions()[i]) ++count;
    QCOMPARE(count,changed);
    QTest::mouseClick(editor.findChild<QPushButton*>("retopoUndo"),Qt::LeftButton);
    QVERIFY(before==editor.document().positions());
  }
  void mouse_face_move_undo_and_original() {
    p3dgui::RetopoMeshEditor editor(true);
    editor.resize(900,700);
    editor.setMesh({{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {{0,1,2,3}});
    editor.show();
    editor.viewport().setPreset(pixel3d::ViewRole::Front);
    QTest::qWait(150);
    editor.findChild<QComboBox*>("retopoSelection")->setCurrentIndex(2);
    const auto p=editor.viewport().project(editor.viewport().scene().solid->positions[0]*.25+
        editor.viewport().scene().solid->positions[1]*.25+editor.viewport().scene().solid->positions[2]*.25+
        editor.viewport().scene().solid->positions[3]*.25).toPoint();
    auto* surface=editor.viewport().surface();
    QTest::mouseClick(surface,Qt::LeftButton,Qt::NoModifier,p);
    editor.findChild<QComboBox*>("retopoTransform")->setCurrentIndex(1);
    const auto before=editor.document().positions();
    QTest::mousePress(surface,Qt::LeftButton,Qt::NoModifier,p);
    QTest::mouseMove(surface,p+QPoint(40,0),30);
    QTest::mouseRelease(surface,Qt::LeftButton,Qt::NoModifier,p+QPoint(40,0));
    QVERIFY(editor.document().positions()!=before);
    QCOMPARE(editor.document().faces().size(),size_t(1));
    QVERIFY(editor.findChild<QPushButton*>("retopoUndo")->isEnabled());
    QTest::mouseClick(editor.findChild<QPushButton*>("retopoUndo"),Qt::LeftButton);
    QVERIFY(editor.document().positions()==before);
    QTest::mouseClick(editor.findChild<QPushButton*>("retopoRedo"),Qt::LeftButton);
    QVERIFY(editor.document().positions()!=before);
    editor.findChild<QComboBox*>("retopoComparison")->setCurrentIndex(1);
    QVERIFY(!editor.findChild<QComboBox*>("retopoTransform")->isEnabled());
    editor.findChild<QComboBox*>("retopoComparison")->setCurrentIndex(0);
    QTest::mouseClick(editor.findChild<QPushButton*>("retopoRevert"),Qt::LeftButton);
    QVERIFY(editor.document().positions()==before);
  }
};
THREERE_TEST(RetopoEditorTests)
#include "test_retopo_editor.moc"
