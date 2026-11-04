#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "backends/trellis_backend.hpp"
#include "registry.hpp"

namespace {
const QString kSha(64, QChar('a'));
void write_json(const QString& file, const QJsonObject& o) {
  QFile f(file);
  QVERIFY(f.open(QIODevice::WriteOnly));
  f.write(QJsonDocument(o).toJson());
}
QJsonObject trellis_block() {
  return {{"runtime_dir", "runtime"}, {"models_dir", "../models/trellis/q4"},
          {"post_executable", "runtime/pixel3d-trellis-post-safe.exe"},
          {"weights_manifest", "../models/trellis/models-manifest.json"},
          {"engine_sha256", kSha}, {"post_sha256", kSha}};
}
}  // namespace

class ConfigTests : public QObject {
  Q_OBJECT
 private slots:
  void nested_config_relative_paths() {
    QTemporaryDir dir;
    QDir().mkpath(dir.filePath("app"));
    const QString file = dir.filePath("app/3re.config.json");
    write_json(file, {{"format", "3re-config"}, {"trellis", trellis_block()}});
    const auto c = p3dasset::BackendConfig::read(file);
    QCOMPARE(QDir::cleanPath(c.runtime_dir), QDir::cleanPath(dir.filePath("app/runtime")));
    QCOMPARE(QDir::cleanPath(c.models_dir), QDir::cleanPath(dir.filePath("models/trellis/q4")));
    QCOMPARE(QDir::cleanPath(c.post_executable), QDir::cleanPath(dir.filePath("app/runtime/pixel3d-trellis-post-safe.exe")));
  }
  void legacy_flat_config_still_reads() {
    QTemporaryDir dir;
    const QString file = dir.filePath("trellis-backend.local.json");
    write_json(file, trellis_block());
    const auto c = p3dasset::BackendConfig::read(file);
    QCOMPARE(QDir::cleanPath(c.runtime_dir), QDir::cleanPath(dir.filePath("runtime")));
  }
  void sam_config_relative() {
    QTemporaryDir dir;
    const QString file = dir.filePath("3re.config.json");
    write_json(file, {{"trellis", trellis_block()}, {"sam", QJsonObject{{"dir", "../models/sam2.1-tiny"}, {"onnxruntime", "onnxruntime.dll"}}}});
    const auto s = p3dasset::SamConfig::read(file);
    QCOMPARE(QDir::cleanPath(s.dir), QDir::cleanPath(dir.filePath("../models/sam2.1-tiny")));
    QCOMPARE(QDir::cleanPath(s.onnxruntime), QDir::cleanPath(dir.filePath("onnxruntime.dll")));
  }
  void sam_config_missing_block_is_empty() {
    QTemporaryDir dir;
    const QString file = dir.filePath("3re.config.json");
    write_json(file, {{"trellis", trellis_block()}});
    QVERIFY(p3dasset::SamConfig::read(file).dir.isEmpty());
  }
  void find_prefers_env_then_appdir() {
    QTemporaryDir dir;
    const QString appConfig = dir.filePath("3re.config.json"), envConfig = dir.filePath("env.json");
    write_json(appConfig, {{"trellis", trellis_block()}});
    write_json(envConfig, {{"trellis", trellis_block()}});
    qputenv("THREERE_TRELLIS_CONFIG", envConfig.toUtf8());
    QCOMPARE(p3dasset::find_backend_config(dir.path()), envConfig);
    qunsetenv("THREERE_TRELLIS_CONFIG");
    QCOMPARE(p3dasset::find_backend_config(dir.path()), appConfig);
    QFile::remove(appConfig);
    QTemporaryDir empty;
    QVERIFY(p3dasset::find_backend_config(empty.path()).isEmpty());
  }
};
THREERE_TEST(ConfigTests)
#include "test_config.moc"
