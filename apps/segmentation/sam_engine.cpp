#include "segmentation/sam_engine.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "segmentation/mask_set.hpp"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

// The header spells the calling convention the MSVC way; MinGW knows __stdcall (ignored on x64).
#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_c_api.h>

namespace p3dseg {
namespace {

constexpr int kInput = 1024;
const char* kEncoderFile = "sam2.1_hiera_tiny.encoder.onnx";
const char* kDecoderFile = "sam2.1_hiera_tiny.decoder.onnx";

qint64 working_set() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return qint64(pmc.WorkingSetSize);
#endif
  return 0;
}

}  // namespace

struct SamEngine::Impl {
#ifdef Q_OS_WIN
  HMODULE dll = nullptr;
#endif
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSessionOptions* options = nullptr;
  OrtSession* enc = nullptr;
  OrtSession* dec = nullptr;
  OrtMemoryInfo* mem = nullptr;
  SamSignature enc_sig, dec_sig;
  std::vector<float> embed, hf0, hf1;  // encoder outputs for the current picture
  QSize working;
  bool has_image = false;
  SamMetrics metrics;
  qint64 rss_before = 0;
  QString error;

  ~Impl() { release(); }

  void release() {
    if (api) {
      if (enc) api->ReleaseSession(enc);
      if (dec) api->ReleaseSession(dec);
      if (options) api->ReleaseSessionOptions(options);
      if (mem) api->ReleaseMemoryInfo(mem);
      if (env) api->ReleaseEnv(env);
    }
    enc = dec = nullptr;
    options = nullptr;
    mem = nullptr;
    env = nullptr;
    api = nullptr;
    // onnxruntime.dll stays loaded for the life of the process (unloading it is not supported cleanly).
  }

  void check(OrtStatus* s) {
    if (!s) return;
    const std::string msg = api->GetErrorMessage(s);
    api->ReleaseStatus(s);
    throw std::runtime_error(msg);
  }

  SamSignature signature(OrtSession* s) {
    SamSignature sig;
    OrtAllocator* alloc = nullptr;
    check(api->GetAllocatorWithDefaultOptions(&alloc));
    auto read = [&](bool input) {
      size_t n = 0;
      check(input ? api->SessionGetInputCount(s, &n) : api->SessionGetOutputCount(s, &n));
      for (size_t i = 0; i < n; ++i) {
        char* name = nullptr;
        check(input ? api->SessionGetInputName(s, i, alloc, &name) : api->SessionGetOutputName(s, i, alloc, &name));
        const QString qn = QString::fromUtf8(name);
        check(api->AllocatorFree(alloc, name));
        OrtTypeInfo* type = nullptr;
        check(input ? api->SessionGetInputTypeInfo(s, i, &type) : api->SessionGetOutputTypeInfo(s, i, &type));
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        check(api->CastTypeInfoToTensorInfo(type, &tensor));
        size_t dims = 0;
        QVector<qint64> shape;
        if (tensor) {
          check(api->GetDimensionsCount(tensor, &dims));
          std::vector<int64_t> v(dims);
          check(api->GetDimensions(tensor, v.data(), dims));
          for (int64_t x : v) shape << x;
        }
        api->ReleaseTypeInfo(type);
        (input ? sig.inputs : sig.outputs) << qn;
        sig.shapes.insert(qn, shape);
      }
    };
    read(true);
    read(false);
    return sig;
  }

  OrtValue* tensor(std::vector<float>& data, const std::vector<int64_t>& shape) {
    OrtValue* v = nullptr;
    check(api->CreateTensorWithDataAsOrtValue(mem, data.data(), data.size() * sizeof(float), shape.data(), shape.size(),
                                              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v));
    return v;
  }
};

SamEngine::SamEngine() : d_(std::make_unique<Impl>()) {}
SamEngine::~SamEngine() = default;

bool SamEngine::load(const QString& dir, const QString& ortDll, QString* error) {
  d_ = std::make_unique<Impl>();
  auto fail = [&](const QString& why) {
    d_->release();
    d_->error = why;
    if (error) *error = why;
    return false;
  };
  const QString encoder = QDir(dir).filePath(kEncoderFile), decoder = QDir(dir).filePath(kDecoderFile);
  if (!QFileInfo(encoder).isFile() || !QFileInfo(decoder).isFile())
    return fail(QObject::tr("SAM 2.1-tiny model files are missing in %1.").arg(QDir::toNativeSeparators(dir)));
  QElapsedTimer timer;
  timer.start();
  d_->rss_before = working_set();
#ifdef Q_OS_WIN
  // Full path + altered search path: its Visual C++ runtime is found next to it.
  d_->dll = LoadLibraryExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(ortDll).utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!d_->dll) return fail(QObject::tr("ONNX Runtime could not be loaded from %1 (Windows error %2).").arg(QDir::toNativeSeparators(ortDll)).arg(GetLastError()));
  using GetBase = const OrtApiBase*(ORT_API_CALL*)();
  auto get_base = reinterpret_cast<GetBase>(reinterpret_cast<void*>(GetProcAddress(d_->dll, "OrtGetApiBase")));
  if (!get_base) return fail(QObject::tr("%1 is not ONNX Runtime.").arg(QDir::toNativeSeparators(ortDll)));
  d_->api = get_base()->GetApi(ORT_API_VERSION);
  if (!d_->api) return fail(QObject::tr("ONNX Runtime %1 is older than the version 3re needs (API %2).").arg(QString::fromUtf8(get_base()->GetVersionString())).arg(ORT_API_VERSION));
#else
  return fail(QObject::tr("Segmentation with SAM needs Windows in this version."));
#endif
  try {
    auto& api = *d_->api;
    d_->check(api.CreateEnv(ORT_LOGGING_LEVEL_ERROR, "3re-sam", &d_->env));
    d_->check(api.CreateSessionOptions(&d_->options));
    const int threads = std::max(1, int(std::thread::hardware_concurrency()) / 2);
    d_->metrics.threads = threads;
    d_->check(api.SetIntraOpNumThreads(d_->options, threads));
    d_->check(api.SetSessionGraphOptimizationLevel(d_->options, ORT_ENABLE_ALL));
    d_->check(api.CreateSession(d_->env, reinterpret_cast<const ORTCHAR_T*>(QDir::toNativeSeparators(encoder).utf16()), d_->options, &d_->enc));
    d_->check(api.CreateSession(d_->env, reinterpret_cast<const ORTCHAR_T*>(QDir::toNativeSeparators(decoder).utf16()), d_->options, &d_->dec));
    d_->check(api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &d_->mem));
    d_->enc_sig = d_->signature(d_->enc);
    d_->dec_sig = d_->signature(d_->dec);
  } catch (const std::exception& e) {
    return fail(QObject::tr("SAM could not be loaded: %1").arg(QString::fromUtf8(e.what())));
  }
  // The exact signature this engine feeds (docs/reports/3re/sam_verification.md).
  const auto& e = d_->enc_sig;
  const auto& dc = d_->dec_sig;
  QStringList problems;
  if (e.inputs != QStringList{"image"} || e.shapes.value("image") != QVector<qint64>{1, 3, kInput, kInput}) problems << "encoder input";
  for (const char* n : {"image_embed", "high_res_feats_0", "high_res_feats_1"})
    if (!e.outputs.contains(n)) problems << QStringLiteral("encoder output %1").arg(n);
  if (e.shapes.value("image_embed") != QVector<qint64>{1, 256, 64, 64} || e.shapes.value("high_res_feats_0") != QVector<qint64>{1, 32, 256, 256} ||
      e.shapes.value("high_res_feats_1") != QVector<qint64>{1, 64, 128, 128})
    problems << "encoder output shapes";
  for (const char* n : {"image_embed", "high_res_feats_0", "high_res_feats_1", "point_coords", "point_labels", "mask_input", "has_mask_input"})
    if (!dc.inputs.contains(n)) problems << QStringLiteral("decoder input %1").arg(n);
  if (!dc.outputs.contains("masks") || !dc.outputs.contains("iou_predictions")) problems << "decoder outputs";
  if (!problems.isEmpty()) return fail(QObject::tr("The SAM model files differ from SAM 2.1-tiny as 3re uses it (%1).").arg(problems.join(", ")));
  d_->metrics.load_ms = double(timer.nsecsElapsed()) / 1e6;
  if (error) error->clear();
  return true;
}

bool SamEngine::ready() const { return d_->enc && d_->dec; }
SamSignature SamEngine::encoderSignature() const { return d_->enc_sig; }
SamSignature SamEngine::decoderSignature() const { return d_->dec_sig; }
bool SamEngine::hasImage() const { return d_->has_image; }
QSize SamEngine::workingSize() const { return d_->working; }
SamMetrics SamEngine::metrics() const { return d_->metrics; }
QString SamEngine::lastError() const { return d_->error; }

bool SamEngine::setImage(const QImage& image, QString* error) {
  d_->has_image = false;
  if (!ready() || image.isNull()) {
    if (error) *error = QObject::tr("SAM is not loaded.");
    return false;
  }
  QElapsedTimer timer;
  timer.start();
  d_->working = working_size(image.size());
  // RGB at 1024 x 1024, ImageNet mean/std, CHW (the export's preprocessing).
  const QImage rgb = image.convertToFormat(QImage::Format_RGB888).scaled(kInput, kInput, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  std::vector<float> input(size_t(3) * kInput * kInput);
  const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
  for (int y = 0; y < kInput; ++y) {
    const uchar* row = rgb.constScanLine(y);
    for (int x = 0; x < kInput; ++x)
      for (int c = 0; c < 3; ++c) input[size_t(c) * kInput * kInput + size_t(y) * kInput + x] = (row[x * 3 + c] / 255.0f - mean[c]) / stdev[c];
  }
  try {
    auto& api = *d_->api;
    OrtValue* in = d_->tensor(input, {1, 3, kInput, kInput});
    const char* in_names[] = {"image"};
    const QList<QByteArray> outs{"image_embed", "high_res_feats_0", "high_res_feats_1"};
    const char* out_names[] = {outs[0].constData(), outs[1].constData(), outs[2].constData()};
    OrtValue* out[3] = {nullptr, nullptr, nullptr};
    OrtStatus* s = api.Run(d_->enc, nullptr, in_names, &in, 1, out_names, 3, out);
    api.ReleaseValue(in);
    d_->check(s);
    std::vector<float>* dst[3] = {&d_->embed, &d_->hf0, &d_->hf1};
    const size_t sizes[3] = {size_t(256) * 64 * 64, size_t(32) * 256 * 256, size_t(64) * 128 * 128};
    for (int i = 0; i < 3; ++i) {
      float* data = nullptr;
      d_->check(api.GetTensorMutableData(out[i], reinterpret_cast<void**>(&data)));
      dst[i]->assign(data, data + sizes[i]);
      api.ReleaseValue(out[i]);
    }
  } catch (const std::exception& e) {
    d_->error = QObject::tr("SAM could not analyse the picture: %1").arg(QString::fromUtf8(e.what()));
    if (error) *error = d_->error;
    return false;
  }
  d_->metrics.encode_ms = double(timer.nsecsElapsed()) / 1e6;
  d_->metrics.rss_delta_bytes = working_set() - d_->rss_before;
  d_->has_image = true;
  return true;
}

QImage SamEngine::predict(const QVector<QPointF>& pos, const QVector<QPointF>& neg, std::optional<QRectF> box) {
  if (!ready() || !d_->has_image || (pos.isEmpty() && !box)) return {};
  QElapsedTimer timer;
  timer.start();
  const double sx = double(kInput) / d_->working.width(), sy = double(kInput) / d_->working.height();
  std::vector<float> coords, labels;
  auto add = [&](QPointF p, float label) {
    coords.push_back(float(p.x() * sx));
    coords.push_back(float(p.y() * sy));
    labels.push_back(label);
  };
  for (const QPointF& p : pos) add(p, 1);
  for (const QPointF& p : neg) add(p, 0);
  if (box) {
    add(box->topLeft(), 2);
    add(box->bottomRight(), 3);
  } else {
    add(QPointF(0, 0), -1);  // padding point (SAM prompt convention without a box)
  }
  const int64_t n = int64_t(labels.size());
  std::vector<float> mask_input(size_t(256) * 256, 0.f), has_mask(1, 0.f);
  QImage result;
  try {
    auto& api = *d_->api;
    std::vector<OrtValue*> in{d_->tensor(d_->embed, {1, 256, 64, 64}), d_->tensor(d_->hf0, {1, 32, 256, 256}),
                              d_->tensor(d_->hf1, {1, 64, 128, 128}), d_->tensor(coords, {1, n, 2}), d_->tensor(labels, {1, n}),
                              d_->tensor(mask_input, {1, 1, 256, 256}), d_->tensor(has_mask, {1})};
    const char* in_names[] = {"image_embed", "high_res_feats_0", "high_res_feats_1", "point_coords", "point_labels", "mask_input", "has_mask_input"};
    const char* out_names[] = {"masks", "iou_predictions"};
    OrtValue* out[2] = {nullptr, nullptr};
    OrtStatus* s = api.Run(d_->dec, nullptr, in_names, in.data(), in.size(), out_names, 2, out);
    for (OrtValue* v : in) api.ReleaseValue(v);
    d_->check(s);
    OrtTensorTypeAndShapeInfo* info = nullptr;
    d_->check(api.GetTensorTypeAndShape(out[0], &info));
    size_t dims = 0;
    d_->check(api.GetDimensionsCount(info, &dims));
    std::vector<int64_t> shape(dims);
    d_->check(api.GetDimensions(info, shape.data(), dims));
    api.ReleaseTensorTypeAndShapeInfo(info);
    if (dims != 4) throw std::runtime_error("unexpected mask shape");
    const int candidates = int(shape[1]), mh = int(shape[2]), mw = int(shape[3]);
    float *masks = nullptr, *scores = nullptr;
    d_->check(api.GetTensorMutableData(out[0], reinterpret_cast<void**>(&masks)));
    d_->check(api.GetTensorMutableData(out[1], reinterpret_cast<void**>(&scores)));
    const int best = int(std::max_element(scores, scores + candidates) - scores);
    QImage low(mw, mh, QImage::Format_Grayscale8);
    const float* m = masks + size_t(best) * mh * mw;
    for (int y = 0; y < mh; ++y) {
      uchar* row = low.scanLine(y);
      for (int x = 0; x < mw; ++x) row[x] = uchar(std::lround(255.0 / (1.0 + std::exp(-double(m[size_t(y) * mw + x])))));
    }
    api.ReleaseValue(out[0]);
    api.ReleaseValue(out[1]);
    // Probabilities upscaled smoothly, then cut at 0.5 (= logit 0).
    result = low.scaled(d_->working, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < result.height(); ++y) {
      uchar* row = result.scanLine(y);
      for (int x = 0; x < result.width(); ++x) row[x] = row[x] > 127 ? 255 : 0;
    }
  } catch (const std::exception& e) {
    d_->error = QObject::tr("SAM could not select the region: %1").arg(QString::fromUtf8(e.what()));
    return {};
  }
  d_->metrics.decode_ms = double(timer.nsecsElapsed()) / 1e6;
  return result;
}

}  // namespace p3dseg
