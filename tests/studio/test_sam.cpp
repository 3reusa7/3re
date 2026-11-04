#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include "registry.hpp"
#include "segmentation/sam_engine.hpp"

namespace {
const QString kFixture = QStringLiteral(PIXEL3D_FIXTURES_DIR "/S_segmentation");
double iou(const QImage& a, const QImage& b) {
  long inter = 0, uni = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      const bool p = a.constScanLine(y)[x] > 127, q = b.constScanLine(y)[x] > 127;
      inter += p && q;
      uni += p || q;
    }
  return uni ? double(inter) / uni : 0;
}
}  // namespace

class SamTests : public QObject {
  Q_OBJECT
  p3dseg::SamEngine sam_;
  QJsonObject report_;

 private slots:
  void initTestCase() {
    QString error;
    QVERIFY2(sam_.load(QStringLiteral(THREERE_SAM_DIR), QStringLiteral(THREERE_ORT_DLL), &error), qPrintable(error));
    QVERIFY(sam_.ready());
  }
  void sam_signature_matches_expected() {
    const auto e = sam_.encoderSignature(), d = sam_.decoderSignature();
    QCOMPARE(e.inputs, QStringList{"image"});
    QCOMPARE(e.shapes.value("image"), (QVector<qint64>{1, 3, 1024, 1024}));
    QVERIFY(e.outputs.contains("image_embed") && e.outputs.contains("high_res_feats_0") && e.outputs.contains("high_res_feats_1"));
    QCOMPARE(e.shapes.value("image_embed"), (QVector<qint64>{1, 256, 64, 64}));
    for (const char* n : {"image_embed", "high_res_feats_0", "high_res_feats_1", "point_coords", "point_labels", "mask_input", "has_mask_input"})
      QVERIFY2(d.inputs.contains(n), n);
    QVERIFY(d.outputs.contains("masks") && d.outputs.contains("iou_predictions"));
  }
  void sam_click_iou() {
    QString error;
    const QImage img(kFixture + "/subject.png");
    QVERIFY(!img.isNull());
    QVERIFY2(sam_.setImage(img, &error), qPrintable(error));
    QCOMPARE(sam_.workingSize(), img.size());
    QFile clicks(kFixture + "/clicks.txt");
    QVERIFY(clicks.open(QIODevice::ReadOnly | QIODevice::Text));
    QTextStream in(&clicks);
    QJsonObject parts;
    while (!in.atEnd()) {
      const QStringList f = in.readLine().split(' ', Qt::SkipEmptyParts);
      if (f.size() != 3) continue;
      const QImage gt = QImage(kFixture + "/" + f[0] + "_mask.png").convertToFormat(QImage::Format_Grayscale8);
      const QImage m = sam_.predict({QPointF(f[1].toDouble(), f[2].toDouble())}, {}, std::nullopt);
      QCOMPARE(m.size(), gt.size());
      const double score = iou(m, gt);
      parts[f[0]] = QJsonObject{{"iou", score}, {"decode_ms", sam_.metrics().decode_ms}};
      QVERIFY2(score >= 0.80, qPrintable(QString("%1 IoU %2 < 0.80").arg(f[0]).arg(score)));
    }
    const auto m = sam_.metrics();
    report_ = QJsonObject{{"load_ms", m.load_ms}, {"encode_ms", m.encode_ms}, {"rss_delta_mib", double(m.rss_delta_bytes) / 1048576.0}, {"parts", parts}};
    qInfo().noquote() << "SAM metrics:" << QJsonDocument(report_).toJson(QJsonDocument::Compact);
    const QString out = qEnvironmentVariable("THREERE_SAM_REPORT");
    if (!out.isEmpty()) {
      QFile f(out);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QJsonDocument(report_).toJson());
    }
  }
  void sam_negative_point_shrinks() {
    QString error;
    QVERIFY(sam_.setImage(QImage(kFixture + "/subject.png"), &error));
    const QImage both = sam_.predict({QPointF(300, 180), QPointF(320, 390)}, {}, std::nullopt);
    const QImage ball = sam_.predict({QPointF(300, 180)}, {QPointF(320, 390)}, std::nullopt);
    auto count = [](const QImage& m) {
      long n = 0;
      for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) n += m.constScanLine(y)[x] > 127;
      return n;
    };
    QVERIFY(count(ball) < count(both));
  }
  void sam_missing_model_reports() {
    p3dseg::SamEngine other;
    QTemporaryDir empty;
    QString error;
    QVERIFY(!other.load(empty.path(), QStringLiteral(THREERE_ORT_DLL), &error));
    QVERIFY2(error.contains("missing", Qt::CaseInsensitive), qPrintable(error));
    QVERIFY(!other.ready());
    QVERIFY(other.predict({QPointF(1, 1)}, {}, std::nullopt).isNull());
    QVERIFY(!other.load(QStringLiteral(THREERE_SAM_DIR), empty.filePath("onnxruntime.dll"), &error));
    QVERIFY2(error.contains("ONNX Runtime", Qt::CaseInsensitive), qPrintable(error));
  }
};
THREERE_TEST(SamTests)
#include "test_sam.moc"
