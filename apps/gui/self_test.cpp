#include "self_test.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <filesystem>

#include "agent/agent_godot.hpp"
#include "backends/trellis_backend.hpp"
#include "backends/trellis_diagnostics.hpp"
#include "segmentation/sam_engine.hpp"

namespace p3dgui {
namespace {

QString sha256_file(const QString& file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData(&f);
  return QString::fromLatin1(h.result().toHex());
}

// Every string value of the config, with its key path ("trellis.runtime_dir").
void collect_strings(const QJsonObject& o, const QString& prefix, QList<QPair<QString, QString>>& out) {
  for (auto it = o.begin(); it != o.end(); ++it) {
    const QString key = prefix.isEmpty() ? it.key() : prefix + "." + it.key();
    if (it->isObject()) collect_strings(it->toObject(), key, out);
    else if (it->isString()) out.append({key, it->toString()});
  }
}

bool looks_like_path(const QString& key) {
  return key.endsWith("_dir") || key.endsWith("_executable") || key.endsWith("_manifest") || key.endsWith(".dir") ||
         key.endsWith("onnxruntime");
}

}  // namespace

QList<SelfTestItem> run_self_test(const QString& appDir, bool deep) {
  QList<SelfTestItem> items;
  auto add = [&](const QString& name, bool ok, const QString& detail, bool fatal = true) {
    items.append({name, ok, detail, fatal});
  };
  const QDir app(appDir);
  add("qt_platform", !QGuiApplication::platformName().isEmpty(), QGuiApplication::platformName());

  const QString configFile = app.filePath("3re.config.json");
  QJsonObject raw;
  {
    QFile f(configFile);
    if (f.open(QIODevice::ReadOnly)) raw = QJsonDocument::fromJson(f.readAll()).object();
  }
  p3dasset::BackendConfig config;
  bool configOk = false;
  if (raw.isEmpty()) {
    add("config", false, QStringLiteral("%1 is missing or not valid JSON").arg(QDir::toNativeSeparators(configFile)));
  } else {
    try {
      config = p3dasset::BackendConfig::read(configFile);
      configOk = true;
      add("config", true, QDir::toNativeSeparators(configFile));
    } catch (const std::exception& e) {
      add("config", false, QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(configFile), QString::fromUtf8(e.what())));
    }
  }

  // A portable release refers only to its own folder.
  QList<QPair<QString, QString>> strings;
  collect_strings(raw, {}, strings);
  QStringList foreign;
  for (const auto& [key, value] : strings) {
    if (!looks_like_path(key)) continue;
    if (QDir::isAbsolutePath(value) || value.contains("Codex", Qt::CaseInsensitive) || value.contains(".claude"))
      foreign << key + " = " + value;
  }
  add("no_foreign_paths", !raw.isEmpty() && foreign.isEmpty(),
      foreign.isEmpty() ? QStringLiteral("all paths relative to the release") : foreign.join("; "));

  for (const char* helper : {"pixel3d-asset-inspect.exe", "pixel3d-geometry-audit.exe", "pixel3d-cuda-health.exe", "pixel3d-auto-texture.exe",
                             "3re-agent.exe", "pixel3d-decimate-guarded.exe", "gltf_validator.exe", "3re-chat.exe", "pixel3d-regional-fix.exe",
                             "3re-update.exe"})
    if (!QFileInfo(app.filePath(helper)).isFile()) add("helpers", false, QStringLiteral("missing %1").arg(helper));
  if (std::none_of(items.begin(), items.end(), [](const SelfTestItem& i) { return i.name == "helpers"; }))
    add("helpers", true, QStringLiteral("asset inspect, geometry audit, CUDA probe, auto texture"));

  // 2.10.0: the agent's delivery verification is a real Godot 4 import; without it no asset can PASS, so a
  // release that cannot run it fails. --deep imports a one-triangle GLB headless.
  {
    const auto g = p3dagent::locate_godot();
    if (!g.found()) {
      add("delivery_verification", false, QString::fromStdString(p3dagent::describe_godot(g)), deep);  // users may not have Godot: only the deep release check needs it
    } else if (deep) {
      const auto work = std::filesystem::temp_directory_path() / ("3re-self-test-godot-" + std::to_string(QCoreApplication::applicationPid()));
      const auto smoke = p3dagent::godot_smoke_test(g.exe, work);
      std::error_code ec;
      std::filesystem::remove_all(work, ec);
      add("delivery_verification", smoke.ok,
          QStringLiteral("Godot %1 (%2, %3): %4 (%5 s)")
              .arg(QString::fromStdString(g.version), QString::fromStdString(g.source), QDir::toNativeSeparators(QString::fromStdWString(g.exe.wstring())),
                   QString::fromStdString(smoke.detail))
              .arg(smoke.seconds, 0, 'f', 1));
    } else {
      add("delivery_verification", true, QString::fromStdString(p3dagent::describe_godot(g)));
    }
  }

  if (configOk) {
    // The engine program ships with 3re; its GPU libraries, the weights and SAM are downloaded on first use
    // (app/models.json, Settings > AI models): absent ones are a warning, a damaged one fails a deep check.
    const QString later = QStringLiteral("not downloaded yet (Settings > AI models)");
    add("runtime_files", QFileInfo(config.engine()).isFile(), QDir::toNativeSeparators(config.engine()));
    QStringList libsMissing;
    for (const char* name : {"ggml.dll", "ggml-base.dll", "ggml-cpu.dll", "ggml-cuda.dll", "cudart64_12.dll", "cublas64_12.dll", "cublasLt64_12.dll"})
      if (!QFileInfo(QDir(config.runtime_dir).filePath(name)).isFile()) libsMissing << name;
    add("engine_libraries", libsMissing.isEmpty(), libsMissing.isEmpty() ? QStringLiteral("GPU engine libraries present")
                                                   : libsMissing.size() == 7 ? later : "incomplete: missing " + libsMissing.join(", "),
        false);
    add("engine_sha256", sha256_file(config.engine()) == config.engine_sha256, QFileInfo(config.engine()).fileName());
    add("post_sha256", sha256_file(config.post_executable) == config.post_sha256, QFileInfo(config.post_executable).fileName());

    QStringList missingModels, damagedModels;
    QFile manifest(config.weights_manifest);
    int listed = 0;
    if (manifest.open(QIODevice::ReadOnly)) {
      const auto files = QJsonDocument::fromJson(manifest.readAll()).object().value("files").toArray();
      listed = int(files.size());
      for (const auto& v : files) {
        const auto e = v.toObject();
        const QString file = QDir(config.models_dir).filePath(QFileInfo(e.value("path").toString()).fileName());
        const QFileInfo info(file);
        if (!info.isFile()) missingModels << info.fileName();
        else if (info.size() != qint64(e.value("size").toDouble())) damagedModels << "incomplete " + info.fileName();
        else if (deep && sha256_file(file) != e.value("lfs").toObject().value("oid").toString()) damagedModels << "checksum " + info.fileName();
      }
    }
    if (!listed) add("trellis_models", false, QStringLiteral("the weights manifest is missing or empty"));
    else if (!damagedModels.isEmpty()) add("trellis_models", false, damagedModels.join(", "), deep);
    else if (!missingModels.isEmpty()) add("trellis_models", false, missingModels.size() == listed ? later : "missing " + missingModels.join(", "), false);
    else add("trellis_models", true, deep ? QStringLiteral("sizes and SHA-256 match the manifest") : QStringLiteral("sizes match the manifest"));

    const auto sam = p3dasset::SamConfig::read(configFile);
    QStringList samMissing;
    for (const char* name : {"sam2.1_hiera_tiny.encoder.onnx", "sam2.1_hiera_tiny.decoder.onnx"})
      if (sam.dir.isEmpty() || !QFileInfo(QDir(sam.dir).filePath(name)).isFile()) samMissing << name;
    add("sam_models", samMissing.isEmpty(), samMissing.isEmpty() ? QDir::toNativeSeparators(sam.dir) : samMissing.size() == 2 ? later : "missing " + samMissing.join(", "),
        false);
    add("onnxruntime", !sam.onnxruntime.isEmpty() && QFileInfo(sam.onnxruntime).isFile(), QDir::toNativeSeparators(sam.onnxruntime));
    if (deep && samMissing.isEmpty()) {
      // Loads ONNX Runtime (and its Visual C++ runtime next to it) and checks the model signature.
      p3dseg::SamEngine engine;
      QString error;
      const bool ok = engine.load(sam.dir, sam.onnxruntime, &error);
      add("sam_load", ok, ok ? QStringLiteral("SAM 2.1-tiny loaded and its signature matches (%1 ms, CPU)").arg(engine.metrics().load_ms, 0, 'f', 0) : error);
    }

    const auto health = p3dasset::runCudaHealth(config.runtime_dir);
    add("cuda_probe", health.value("ready").toBool(), health.value("message").toString(), false);
  }
  return items;
}

bool self_test_passed(const QList<SelfTestItem>& items) {
  return std::all_of(items.begin(), items.end(), [](const SelfTestItem& i) { return i.ok || !i.fatal; });
}

QByteArray self_test_json(const QList<SelfTestItem>& items) {
  QJsonArray list;
  for (const auto& i : items) list.append(QJsonObject{{"name", i.name}, {"ok", i.ok}, {"fatal", i.fatal}, {"detail", i.detail}});
  return QJsonDocument(QJsonObject{{"passed", self_test_passed(items)}, {"items", list}}).toJson();
}

}  // namespace p3dgui
