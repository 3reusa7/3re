#include <QTest>

#include "backends/mesh_asset.hpp"
#include "registry.hpp"
#include "studio/export_page.hpp"
#include "studio/preview_bar.hpp"
#include "viewport.hpp"

class ExportPreviewTests : public QObject {
  Q_OBJECT
 private slots:
  void export_page_requires_ack_for_review() {
    p3dgui::ExportPage page;
    int asked = 0, prompted = 0;
    bool answer = false;
    page.setAcknowledgeHandler([&](const QString& text) {
      ++asked;
      return text.contains("REVIEW", Qt::CaseInsensitive) && answer;
    });
    page.setSavePathPrompt([&](const QString&) {
      ++prompted;
      return QString();  // cancel at the file prompt
    });
    p3dasset::MeshAsset asset;
    asset.validation = p3dasset::Validation::Review;
    page.exportAsset(asset, "Knight", {});
    QCOMPARE(asked, 1);
    QCOMPARE(prompted, 0);  // declined: no file prompt, nothing written
    answer = true;
    page.exportAsset(asset, "Knight", {});
    QCOMPARE(asked, 2);
    QCOMPARE(prompted, 1);
    asset.validation = p3dasset::Validation::Pass;
    page.exportAsset(asset, "Knight", {});
    QCOMPARE(asked, 2);  // PASS needs no confirmation
    QCOMPARE(prompted, 2);
  }
  void preview_bar_turntable_toggles() {
    p3dgui::Viewport view(true);
    p3dgui::PreviewBar bar(&view);
    const float yaw = view.state().yaw;
    bar.setTurntable(true);
    QVERIFY(bar.turntable());
    QTRY_VERIFY_WITH_TIMEOUT(view.state().yaw != yaw, 2000);
    bar.setTurntable(false);
    QVERIFY(!bar.turntable());
  }
};
THREERE_TEST(ExportPreviewTests)
#include "test_export_preview.moc"
