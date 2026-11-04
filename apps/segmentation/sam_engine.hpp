#pragma once
// SAM 2.1-tiny on the CPU through ONNX Runtime (C API, onnxruntime.dll loaded at run
// time, so 3re starts and works without it). No GPU memory is used: TRELLIS keeps the
// GPU. The model signature is read from the files and checked at load; timings and
// memory are measured (docs/reports/3re/sam_verification.md). Not thread-safe: call
// from one thread at a time (the editor runs it on a worker thread, one job at a time).
#include <QImage>
#include <QMap>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <optional>

namespace p3dseg {

struct SamSignature {
  QStringList inputs, outputs;
  QMap<QString, QVector<qint64>> shapes;  // -1 = dynamic dimension
};

struct SamMetrics {
  double load_ms = 0, encode_ms = 0, decode_ms = 0;
  qint64 rss_delta_bytes = 0;  // process working set after the last encode minus before load
  int threads = 0;
};

class SamEngine {
 public:
  SamEngine();
  ~SamEngine();
  SamEngine(const SamEngine&) = delete;
  SamEngine& operator=(const SamEngine&) = delete;

  // dir holds sam2.1_hiera_tiny.encoder.onnx and .decoder.onnx; ortDll = onnxruntime.dll.
  bool load(const QString& dir, const QString& ortDll, QString* error);
  bool ready() const;
  SamSignature encoderSignature() const;
  SamSignature decoderSignature() const;

  // Encodes the picture once (about 6 s on a laptop CPU). Masks use workingSize().
  bool setImage(const QImage& image, QString* error);
  bool hasImage() const;
  QSize workingSize() const;
  // Points in working-size pixels: pos = include, neg = exclude; optional box. Returns a
  // Grayscale8 mask (255 inside) at the working size, or a null image when not ready.
  QImage predict(const QVector<QPointF>& pos, const QVector<QPointF>& neg, std::optional<QRectF> box);
  SamMetrics metrics() const;
  QString lastError() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace p3dseg
