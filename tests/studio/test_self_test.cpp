#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include "registry.hpp"
#include "self_test.hpp"

namespace {
const p3dgui::SelfTestItem* item(const QList<p3dgui::SelfTestItem>& items, const QString& name) {
  for (const auto& i : items)
    if (i.name == name) return &i;
  return nullptr;
}
}  // namespace

class SelfTestTests : public QObject {
  Q_OBJECT
 private slots:
  void self_test_reports_missing_runtime() {
    QTemporaryDir dir;
    const auto items = p3dgui::run_self_test(dir.path(), false);
    const auto* config = item(items, "config");
    QVERIFY(config);
    QVERIFY(!config->ok);
    QVERIFY2(config->detail.contains("3re.config.json"), qPrintable(config->detail));
    QVERIFY(item(items, "qt_platform") && item(items, "qt_platform")->ok);
    QVERIFY(!p3dgui::self_test_passed(items));
  }
  void self_test_flags_foreign_paths() {
    QTemporaryDir dir;
    QFile f(dir.filePath("3re.config.json"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"trellis":{"runtime_dir":"C:\Users\x\Documents\Codex\runtime","models_dir":"m","post_executable":"p",)"
            R"("weights_manifest":"w","engine_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
            R"("post_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})");
    f.close();
    const auto items = p3dgui::run_self_test(dir.path(), false);
    const auto* foreign = item(items, "no_foreign_paths");
    QVERIFY(foreign);
    QVERIFY(!foreign->ok);
    QVERIFY(foreign->detail.contains("runtime_dir"));
  }
  // Public releases download the weights, SAM and the GPU libraries on first use: absent is a warning;
  // a damaged file fails the deep check (tools/package.ps1, after an update).
  void self_test_models_not_downloaded_yet_is_a_warning() {
    QTemporaryDir dir;
    const QDir d(dir.path());
    QDir().mkpath(d.filePath("runtime"));
    QDir().mkpath(d.filePath("models"));
    QFile cfg(d.filePath("3re.config.json"));
    QVERIFY(cfg.open(QIODevice::WriteOnly));
    cfg.write(R"({"trellis":{"runtime_dir":"runtime","models_dir":"models","post_executable":"runtime/post.exe",)"
              R"("weights_manifest":"manifest.json","engine_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",)"
              R"("post_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"sam":{"dir":"sam","onnxruntime":"onnxruntime.dll"}})");
    cfg.close();
    QFile manifest(d.filePath("manifest.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"files":[{"path":"q4/dinov3.gguf","size":4,"lfs":{"oid":"0000000000000000000000000000000000000000000000000000000000000000"}}]})");
    manifest.close();
    auto items = p3dgui::run_self_test(dir.path(), false);
    for (const char* name : {"trellis_models", "engine_libraries", "sam_models"}) {
      const auto* i = item(items, name);
      QVERIFY2(i, name);
      QVERIFY2(!i->ok && !i->fatal, name);
      QVERIFY2(i->detail.contains("not downloaded yet"), qPrintable(i->detail));
    }
    QFile weights(d.filePath("models/dinov3.gguf"));
    QVERIFY(weights.open(QIODevice::WriteOnly));
    weights.write("abcd");  // right size, wrong checksum
    weights.close();
    items = p3dgui::run_self_test(dir.path(), false);
    QVERIFY(item(items, "trellis_models")->ok);
    items = p3dgui::run_self_test(dir.path(), true);
    QVERIFY(!item(items, "trellis_models")->ok && item(items, "trellis_models")->fatal);
  }
};
THREERE_TEST(SelfTestTests)
#include "test_self_test.moc"
