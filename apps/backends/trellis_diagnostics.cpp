#include "trellis_diagnostics.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
namespace p3dasset {
QJsonObject cudaDiagnosis(bool environment_ok,bool registered,int code,const QString& name,const QString& error,int devices){
  QString state,message;
  if(!environment_ok){state="gpu_driver_unavailable";message="GPU/driver is unavailable; CUDA runtime was not tested.";}
  else if(!registered){state="backend_registration_missing";message="CUDA backend registration is missing.";}
  else if(code!=0){state="cuda_runtime_initialization_failed";message=QString("CUDA initialization failed: %1 (%2): %3").arg(name).arg(code).arg(error);}
  else if(devices<=0){state="backend_registered_no_devices";message="CUDA is registered, but no usable CUDA devices were enumerated.";}
  else{state="ready";message=QString("CUDA ready: %1 device(s); no inference performed.").arg(devices);}
  return {{"schema",1},{"status",state},{"ready",state=="ready"},{"message",message},
    {"cuda_error_code",environment_ok&&registered?QJsonValue(code):QJsonValue(QJsonValue::Null)},
    {"cuda_error_name",name},{"cuda_error_text",error},{"device_count",devices},{"inference_executed",false}};
}
QJsonObject runCudaHealth(const QString& runtime){
  QProcess child;
#ifdef Q_OS_WIN
  child.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){a->flags|=0x08000000;});
#endif
  const auto exe=QDir(QCoreApplication::applicationDirPath()).filePath("pixel3d-cuda-health.exe");
  child.start(exe,{"--runtime",runtime});
  if(!child.waitForStarted(2000) || !child.waitForFinished(7000)){
    child.kill();child.waitForFinished(1000);
    return {{"ready",false},{"status","probe_failed"},{"message","GPU readiness probe failed or timed out; generation is disabled."}};
  }
  const auto result=QJsonDocument::fromJson(child.readAllStandardOutput()).object();
  if(child.exitStatus()!=QProcess::NormalExit || result.value("schema").toInt()!=1 || !result.value("ready").isBool())
    return {{"ready",false},{"status","probe_failed"},{"message","GPU readiness probe returned an invalid report."}};
  return result;
}
void observeInferenceLine(QJsonObject& r,const QString& line){
  // Pinned verbose engine emits this only at graph execution, not model loading.
  static const QRegularExpression compute(R"(^\[trellis:dbg\]\s+dinov3\s+->\s+.+,\s*\d+ nodes \.\.\.$)");
  if(compute.match(line).hasMatch()){r["inference_started"]=true;r["generation_executed"]=true;}
  if(line.startsWith("ggml_cuda_init: failed to initialize CUDA:")){
    r["cuda_runtime_diagnostic"]=line;
    r["cuda_error_code"]=QJsonValue(QJsonValue::Null); // Legacy engine did not emit a numeric code.
  }
}
void finishInference(QJsonObject& r,bool success,bool outputs){
  r["inference_completed"]=success&&outputs&&r.value("inference_started").toBool();
}
bool observedGpuSample(bool api_ok,bool dedicated_pid,bool shared_pid){return api_ok&&dedicated_pid&&shared_pid;}
bool collectGpuValue(const QString& instance,const QString& prefix,bool valid,qint64 value,quint64& sum){
  if(!valid||!instance.startsWith(prefix))return false;
  sum+=quint64(qMax<qint64>(0,value));return true;
}
QJsonObject nativeTexturePlan(const QString& directory){
  QJsonArray missing;
  for(const auto* n:{"dinov3.gguf","ss_flow.gguf","ss_dec.gguf","shape_flow_512.gguf","shape_dec.gguf","tex_flow_512.gguf","tex_dec.gguf"})
    if(!QFileInfo(QDir(directory).filePath(n)).isFile())missing.append(n);
  // Hybrid texture (spec 2026-10-20-hybrid-texture-design): enabled whenever all seven weights are present;
  // the backend additionally verifies the texture weights' checksums before every generation.
  const bool ready=missing.isEmpty();
  return {{"enabled",ready},{"status",ready?"hybrid_ready":"weights_missing"},{"default",ready?"hybrid_texture":"picture_reconstruction"},
    {"geometry_resolution",512},{"texture_flow_resolution",512},{"missing_models",missing},
    {"candidate_arguments",QJsonArray{"--res","512","--steps","12","--low-vram-4gb"}},
    {"environment",QJsonArray{"TRELLIS_DUMP_POST=<run>/native.post","TRELLIS_STOP_AFTER_POST_DUMP=1"}},
    {"routing","One run: geometry + texture flow -> POST dump (raw mesh + PBR voxels) -> raw.ply/raw.glb (byte-identical mesh) -> GeometrySafe -> audit -> Hybrid Texture (picture where seen, calibrated TRELLIS texture elsewhere)."},
    {"output_contract","Base colour atlas, UV0, valid GLB; geometry unchanged and separately audited; metallic/roughness not exported."},
    {"memory_note","Measured 2026-10-20 (RTX 3050 4 GB): 1121-1241 MiB peak VRAM, 0.9-1.7 GiB peak private RAM, 121-236 s per generation; a very dense object (2.5 M voxels) can hit the engine's RAM reserve, then the backend retries geometry only."}};
}
}
