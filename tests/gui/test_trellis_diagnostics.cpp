#include <QtTest>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QPushButton>
#include <QLabel>
#include <QFile>
#include <QDir>
#include "backends/trellis_diagnostics.hpp"
#include "studio/image3d_workspace.hpp"
using namespace p3dasset;
class DiagnosticsTests:public QObject{
 Q_OBJECT
 private slots:
 void classifications(){
  QCOMPARE(cudaDiagnosis(false,false,0,{},{},0)["status"].toString(),QString("gpu_driver_unavailable"));
  QCOMPARE(cudaDiagnosis(true,false,0,{},{},0)["status"].toString(),QString("backend_registration_missing"));
  QCOMPARE(cudaDiagnosis(true,true,0,{},{},0)["status"].toString(),QString("backend_registered_no_devices"));
  auto failure=cudaDiagnosis(true,true,1,"cudaErrorInvalidValue","invalid argument",0);
  QCOMPARE(failure["status"].toString(),QString("cuda_runtime_initialization_failed"));QCOMPARE(failure["cuda_error_code"].toInt(),1);QVERIFY(failure["message"].toString().contains("invalid argument"));QVERIFY(!failure["ready"].toBool());
  QVERIFY(cudaDiagnosis(true,true,0,"cudaSuccess","no error",1)["ready"].toBool());
 }
 void telemetryRequiresBothPidObservations(){
  QVERIFY(!observedGpuSample(true,false,false));QVERIFY(!observedGpuSample(true,true,false));QVERIFY(!observedGpuSample(false,true,true));QVERIFY(observedGpuSample(true,true,true));
  quint64 sum=0;
  QVERIFY(!collectGpuValue("pid_1234_luid_0","pid_123_",true,999,sum));QCOMPARE(sum,quint64(0));
  QVERIFY(!collectGpuValue("pid_123_luid_0","pid_123_",false,999,sum));
  QVERIFY(collectGpuValue("pid_123_luid_0","pid_123_",true,0,sum));QCOMPARE(sum,quint64(0));
  QVERIFY(collectGpuValue("pid_123_luid_1","pid_123_",true,128,sum));QCOMPARE(sum,quint64(128));
 }
 void initializationFailureIsNotInference(){
  QJsonObject r{{"worker_launched",true},{"inference_started",false},{"generation_executed",false}};
  observeInferenceLine(r,"[2/6] DINOv3 conditioning");
  observeInferenceLine(r,"ggml_cuda_init: failed to initialize CUDA: invalid argument");
  observeInferenceLine(r,"[trellis] no backend named 'CUDA' in this build");
  finishInference(r,false,false);QVERIFY(!r["inference_started"].toBool());QVERIFY(!r["generation_executed"].toBool());QVERIFY(!r["inference_completed"].toBool());QVERIFY(r["cuda_error_code"].isNull());QVERIFY(r["cuda_runtime_diagnostic"].toString().endsWith("invalid argument"));
 }
 void computeAndCompletion(){
  QJsonObject r;observeInferenceLine(r,"[trellis:dbg] dinov3 -> scheduler, 99 nodes ...");QVERIFY(r["inference_started"].toBool());
  finishInference(r,true,false);QVERIFY(!r["inference_completed"].toBool());finishInference(r,true,true);QVERIFY(r["inference_completed"].toBool());
  QJsonObject absent;finishInference(absent,true,true);QVERIFY(!absent["inference_completed"].toBool());
 }
 void texturePlanCannotEnableInference(){
  QTemporaryDir dir;const auto p=nativeTexturePlan(dir.path());QVERIFY(!p["enabled"].toBool());QCOMPARE(p["status"].toString(),QString("weights_missing"));QCOMPARE(p["geometry_resolution"].toInt(),512);QCOMPARE(p["missing_models"].toArray().size(),7);
  for(const char* n:{"dinov3.gguf","ss_flow.gguf","ss_dec.gguf","shape_flow_512.gguf","shape_dec.gguf","tex_flow_512.gguf"}){QFile f(QDir(dir.path()).filePath(n));QVERIFY(f.open(QIODevice::WriteOnly));}
  QVERIFY(!nativeTexturePlan(dir.path())["enabled"].toBool());QCOMPARE(nativeTexturePlan(dir.path())["missing_models"].toArray().first().toString(),QString("tex_dec.gguf"));
  {QFile f(QDir(dir.path()).filePath("tex_dec.gguf"));QVERIFY(f.open(QIODevice::WriteOnly));}
  const auto ready=nativeTexturePlan(dir.path());QVERIFY(ready["enabled"].toBool());QCOMPARE(ready["default"].toString(),QString("hybrid_texture"));QVERIFY(ready["routing"].toString().contains("byte-identical"));
 }
 void guiRemainsUsableWithoutProbe(){
  p3dgui::Image3DWorkspace dialog(true);QVERIFY(!dialog.findChild<QPushButton*>("image3dGenerate")->isEnabled());QVERIFY(dialog.findChild<QPushButton*>("image3dImport")->isEnabled());QVERIFY(dialog.findChild<QPushButton*>("image3dCheckGpu")->isEnabled());QVERIFY(dialog.findChild<QLabel*>("image3dGpuStatus")->text().contains("not checked"));
 }
};
QTEST_MAIN(DiagnosticsTests)
#include "test_trellis_diagnostics.moc"
