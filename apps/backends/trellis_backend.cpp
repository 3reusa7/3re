#include "trellis_backend.hpp"
#include "image_material.hpp"
#include "trellis_diagnostics.hpp"
#include "trellis_native_post.hpp"
#include "settings_env.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <fstream>
#include <queue>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace p3dasset {
namespace {
constexpr quint64 GiB = 1024ull*1024*1024, MiB = 1024ull*1024;
QString path(const std::filesystem::path& p) { return QString::fromStdWString(p.wstring()); }
std::filesystem::path native(const QString& p) { return std::filesystem::path(p.toStdWString()); }
QJsonObject readJson(const QString& file) {
  QFile stream(file);
  if (!stream.open(QIODevice::ReadOnly) || stream.size() > 32*1024*1024) throw std::runtime_error("Missing or oversized report");
  QJsonParseError error;
  auto result = QJsonDocument::fromJson(stream.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !result.isObject()) throw std::runtime_error("Invalid report JSON");
  return result.object();
}
QString sha256(const QString& file, const std::shared_ptr<std::atomic_bool>& cancelled) {
  QFile stream(file); if (!stream.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open required dependency");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!stream.atEnd()) {
    if (cancelled->load()) throw std::runtime_error("Cancelled");
    if (available_ram() < GiB) throw std::runtime_error("Dependency check stopped: 1 GiB free RAM reserve exhausted");
    const auto bytes = stream.read(1024*1024);
    if (bytes.isEmpty() && stream.error() != QFileDevice::NoError) throw std::runtime_error("Dependency read failed");
    hash.addData(bytes);
  }
  return QString::fromLatin1(hash.result().toHex());
}
QString verify(const BackendConfig& c, const std::shared_ptr<std::atomic_bool>& cancelled) {
  try {
    if (sha256(c.engine(),cancelled) != c.engine_sha256 || sha256(c.post_executable,cancelled) != c.post_sha256)
      return "The pinned TRELLIS executable checksum does not match the verified local runtime.";
    const auto manifest = readJson(c.weights_manifest).value("files").toArray();
    const QStringList required{"dinov3.gguf","ss_flow.gguf","ss_dec.gguf","shape_flow_512.gguf","shape_dec.gguf"};
    for (const auto& name : required) {
      QJsonObject entry;
      for (const auto& value : manifest) if (QFileInfo(value.toObject().value("path").toString()).fileName() == name) entry = value.toObject();
      const auto file = QDir(c.models_dir).filePath(name);
      if (entry.isEmpty() || !QFileInfo(file).isFile() || QFileInfo(file).size() != qint64(entry.value("size").toDouble()))
        return "Required Q4 model is missing or incomplete: " + name;
      if (sha256(file,cancelled) != entry.value("lfs").toObject().value("oid").toString())
        return "Required Q4 model checksum mismatch: " + name;
    }
    for (const auto& name : {"ggml.dll","ggml-base.dll","ggml-cpu.dll","ggml-cuda.dll","cudart64_12.dll","cublas64_12.dll","cublasLt64_12.dll"})
      if (!QFileInfo(QDir(c.runtime_dir).filePath(name)).isFile()) return "Missing runtime library: " + QString::fromLatin1(name);
    return {};
  } catch(const std::exception& e) { return QString::fromUtf8(e.what()); }
}
QJsonObject textureWeights(const BackendConfig& c, const std::shared_ptr<std::atomic_bool>& cancelled) {
  try {
    const auto manifest = readJson(c.weights_manifest).value("files").toArray();
    for (const auto& name : {QString("tex_flow_512.gguf"), QString("tex_dec.gguf")}) {
      QJsonObject entry;
      for (const auto& value : manifest) if (QFileInfo(value.toObject().value("path").toString()).fileName() == name) entry = value.toObject();
      const auto file = QDir(c.models_dir).filePath(name);
      if (entry.isEmpty() || !QFileInfo(file).isFile() || QFileInfo(file).size() != qint64(entry.value("size").toDouble()))
        return {{"available", false}, {"reason", "TRELLIS texture weights are not installed (" + name + ")"}};
      if (cancelled && sha256(file, cancelled) != entry.value("lfs").toObject().value("oid").toString())
        return {{"available", false}, {"reason", "TRELLIS texture weight checksum mismatch (" + name + ")"}};
    }
    return {{"available", true}, {"reason", "tex_flow_512.gguf and tex_dec.gguf verified"}};
  } catch (const std::exception& e) {
    return {{"available", false}, {"reason", QString::fromUtf8(e.what())}};
  }
}
bool validAlpha(const QImage& image) {
  bool clear=false, solid=false;
  if (image.isNull() || image.width()>4096 || image.height()>4096) return false;
  const auto rgba=image.convertToFormat(QImage::Format_RGBA8888);
  for (int y=0;y<rgba.height();++y) for (int x=0;x<rgba.width();++x) {
    auto a=rgba.constScanLine(y)[x*4+3]; clear |= a==0; solid |= a>0;
    if(clear && solid)return true;
  }
  return clear && solid;
}
}  // namespace

quint64 available_ram() {
#ifdef Q_OS_WIN
  MEMORYSTATUSEX memory{}; memory.dwLength=sizeof(memory);
  return GlobalMemoryStatusEx(&memory) ? memory.ullAvailPhys : 0;
#else
  return 0;  // The validated backend currently targets Windows/CUDA.
#endif
}
QString BackendConfig::engine() const { return QDir(runtime_dir).filePath("trellis-cli.exe"); }
BackendConfig BackendConfig::read(const QString& file) {
  const auto top=readJson(file);
  const auto j=top.value("trellis").isObject()?top.value("trellis").toObject():top;
  BackendConfig c{j.value("runtime_dir").toString(),j.value("models_dir").toString(),
                  j.value("post_executable").toString(),j.value("weights_manifest").toString(),
                  j.value("engine_sha256").toString(),j.value("post_sha256").toString()};
  if (c.runtime_dir.isEmpty() || c.models_dir.isEmpty() || c.weights_manifest.isEmpty() ||
      c.engine_sha256.size()!=64 || c.post_sha256.size()!=64) throw std::runtime_error("Incomplete TRELLIS backend setup file");
  const QDir base(QFileInfo(file).absolutePath());
  for(auto* value:{&c.runtime_dir,&c.models_dir,&c.post_executable,&c.weights_manifest})
    if(QDir::isRelativePath(*value))*value=base.absoluteFilePath(*value);
  return c;
}
SamConfig SamConfig::read(const QString& file) {
  const auto sam=readJson(file).value("sam").toObject();
  SamConfig c{sam.value("dir").toString(),sam.value("onnxruntime").toString()};
  const QDir base(QFileInfo(file).absolutePath());
  for(auto* value:{&c.dir,&c.onnxruntime})
    if(!value->isEmpty() && QDir::isRelativePath(*value))*value=base.absoluteFilePath(*value);
  return c;
}
QString find_backend_config(const QString& appDir) {
  const QString base=appDir.isEmpty()?QCoreApplication::applicationDirPath():appDir;
  for(const auto& file:{setting_env("TRELLIS_CONFIG"),base+"/3re.config.json",base+"/trellis-backend.local.json",
                         base+"/../../../.toolchain/trellis/backend.json"})
    if(!file.isEmpty() && QFileInfo(file).isFile())return file;
  return {};
}
TrellisBackend::TrellisBackend(QObject* parent):QObject(parent) {
  monitor_.setInterval(1000);
  connect(&monitor_,&QTimer::timeout,this,&TrellisBackend::sampleResources);
  connect(&process_,&QProcess::readyReadStandardOutput,this,&TrellisBackend::readLogs);
  connect(&process_,&QProcess::readyReadStandardError,this,&TrellisBackend::readLogs);
  connect(&process_,&QProcess::finished,this,&TrellisBackend::processDone);
  connect(&process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){
    if(active_ && error==QProcess::FailedToStart) fail("Could not start the local backend: "+process_.errorString());
  });
  connect(&process_,&QProcess::started,this,[this]{
    if(!active_){process_.kill();return;}
    if(!containAndResume()){fail("Could not contain the worker safely.");return;}
    if(stage_==Stage::Generate || stage_==Stage::TextureGenerate)report_["worker_launched"]=true;
  });
#ifdef Q_OS_WIN
  process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){
    a->flags |= CREATE_SUSPENDED|CREATE_NO_WINDOW;
    a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;a->startupInfo->wShowWindow=SW_HIDE;
  });
#endif
  connect(&preview_,&QFutureWatcher<PreparedAsset>::finished,this,[this]{
    if(!active_)return;
    try {
      auto prepared=preview_.result();
      if(texture_request_){if(!prepared.appearance || !prepared.material_error.isEmpty())throw std::runtime_error("Generated PBR preview failed; projection fallback forbidden");prepared.materials["kind"]="trellis_generated";prepared.materials["experimental"]=true;prepared.materials["quality_validated"]=false;}
      asset_.geometry=prepared.geometry;asset_.appearance=prepared.appearance;
      asset_.geometry_glb=native(prepared.textured_glb.isEmpty()?glb_:prepared.textured_glb);
      asset_.geometry_only=!asset_.appearance;
      asset_.materials=prepared.materials;
      report_["material_capabilities"]=QJsonDocument::fromJson(QByteArray::fromStdString(asset_.materials.dump())).object();
      const auto kind=asset_.materials.value("kind",std::string("geometry_only"));
      const bool hybrid=kind=="auto_texture" && report_["auto_texture"].toObject().value("native_fraction").toDouble()>0;
      report_["material_note"]=hybrid
        ?"Hybrid Texture: surfaces seen in the picture use its colours; everything else shows TRELLIS's own texture of the object, colour-calibrated to the picture (model-inferred, not recovered from the picture)."
        :kind=="trellis_generated"
        ?"TRELLIS learned volumetric material. Hidden surfaces are model-inferred. Native preview displays base color; exported PBR maps are retained."
        :kind=="auto_texture"?"Auto Texture: surfaces seen in the picture use its colours; hidden surfaces are reconstructed estimates from nearby visible colours, not recovered detail."
        :kind=="source_projected"?"Approximate XY source-image projection; hidden surfaces are not reconstructed."
        :asset_.geometry_only?"Geometry only.":"Embedded material retained; origin is not certified. Native preview displays base color.";
      if(!prepared.material_error.isEmpty())report_["material_error"]=prepared.material_error;
      const auto raw=QDir(run_dir_).filePath("raw.glb");
      asset_.raw_glb=QFileInfo::exists(raw)?native(raw):std::filesystem::path{};
      asset_.report=native(QDir(run_dir_).filePath("report.json"));
      report_["status"]=validation_name(asset_.validation);
      report_["output_available"]=true;report_["geometry_only"]=asset_.geometry_only;report_["export_glb"]=path(asset_.geometry_glb);
      report_["runtime_seconds"]=timer_.elapsed()/1000.;report_["phases"]=phases_;
      sampleResources();if(!active_)return;
      if(!saveReport())throw std::runtime_error("Could not commit the validation report");
      active_=false;monitor_.stop();stopGpuTelemetry();releaseJob();
      stdout_.close();stderr_.close();telemetry_.close();
      emit progress(100,"Preview ready");
      emit finished(true,QString("Geometry ready · %1 · %2%3").arg(validation_name(asset_.validation),asset_.geometry_only?"geometry only":kind=="trellis_generated"?"generated material":hybrid?"hybrid texture":kind=="auto_texture"?"auto texture":kind=="source_projected"?"approximate source colors":"embedded material",
        report_.value("cleanup_failed").toBool()?" · cleanup failed; raw geometry retained":""));
    } catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
  });
}
TrellisBackend::~TrellisBackend(){
  if(cancelled_)cancelled_->store(true);
  active_=false;monitor_.stop();releaseJob();stopGpuTelemetry();
  if(process_.state()!=QProcess::NotRunning){process_.kill();process_.waitForFinished(1500);}
}
QString TrellisBackend::helper(const QString& name)const {
  return QDir(QCoreApplication::applicationDirPath()).filePath(name+".exe");
}
bool TrellisBackend::begin(const QString& parent,bool inference,quint64 saved_required) {
  if(active_ || process_.state()!=QProcess::NotRunning || prepare_.isRunning() || preview_.isRunning()){
    emit finished(false,"The previous worker is still stopping; try again shortly.");return false;}
  const auto free=available_ram();const auto needed=inference?generation_admission_ram():saved_required?saved_required:5*GiB/2;
  if(free<needed){emit finished(false,QString("Memory preflight: %1 GiB free; %2 GiB required.").arg(free/double(GiB),0,'f',2).arg(needed/double(GiB),0,'f',2));return false;}
  if(!QDir(parent).exists() || QStorageInfo(parent).bytesAvailable()<qint64(4*GiB)){
    emit finished(false,"Choose an existing output location with at least 4 GiB free disk space.");return false;}
  for(const auto& name:{QString("pixel3d-asset-inspect"),QString("pixel3d-geometry-audit")})
    if(!QFileInfo(helper(name)).isFile()){emit finished(false,"The complete Pixel3D TRELLIS bundle is required.");return false;}
  run_dir_=QDir(parent).filePath("Trellis-"+QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss")+"-"+QUuid::createUuid().toString(QUuid::Id128).left(8));
  if(!QDir().mkdir(run_dir_)){emit finished(false,"Could not create a new results directory.");return false;}
  cancelled_=std::make_shared<std::atomic_bool>(false);active_=true;inference_=inference;
  asset_={};texture_request_=false;texture_job_={};texture_args_.clear();textured_.clear();native_texture_=false;
  preview_required_=GiB;
  timer_.start();phases_={};report_={{"schema_version",1},{"status","RUNNING"},{"output_available",false},
    {"generation_executed",false},{"worker_launched",false},{"inference_started",false},{"inference_completed",false},{"inference_requested",inference},{"resolution",512},{"steps",12},{"attention","no-fa"},
    {"material_capabilities",QJsonObject{{"geometry_only",true},{"pbr_validated",false}}}};
  peak_rss_=peak_commit_=peak_gpu_=peak_shared_=0;minimum_free_=free;telemetry_failures_=gpu_samples_=0;stage_=Stage::None;reproject_source_=false;
  stdout_.setFileName(QDir(run_dir_).filePath("stdout.log"));stderr_.setFileName(QDir(run_dir_).filePath("stderr.log"));
  telemetry_.setFileName(QDir(run_dir_).filePath("memory.csv"));
  if(!stdout_.open(QIODevice::WriteOnly|QIODevice::NewOnly) || !stderr_.open(QIODevice::WriteOnly|QIODevice::NewOnly) ||
     !telemetry_.open(QIODevice::WriteOnly|QIODevice::NewOnly)){fail("Could not open output logs.");return false;}
  telemetry_.write("milliseconds,rss_bytes,job_peak_commit_bytes,free_physical_bytes,pid_dedicated_bytes,pid_shared_bytes\n");
  monitor_.start();return true;
}
QStringList TrellisBackend::inferenceArguments(const BackendConfig& c,const QString& image,const QString& output,quint32 seed,const QString& captures,bool native_texture) {
  QStringList args{image,output,"--models",c.models_dir,"--model","trellis","--backend","CUDA","--gpu","0",
    "--res","512","--steps","12","--seed",QString::number(seed),"--no-texture","--require-gpu","--low-vram-4gb","--no-fa","--threads","2","--verbose"};
  if(native_texture){args.removeAll("--no-texture");return args;}  // the engine refuses stage capture with texture
  if(!captures.isEmpty())args<<"--capture-stages"<<captures;
  return args;
}
QJsonObject TrellisBackend::nativeTextureAvailability(const BackendConfig& c){return textureWeights(c,nullptr);}
bool TrellisBackend::generate(const BackendConfig& config,const QImage& cutout,const QString& source,const QString& parent,quint32 seed,bool capture) {
  if(!validAlpha(cutout)){emit finished(false,"Import and review an image with real transparency first.");return false;}
  if(!begin(parent,true))return false;
  config_=config;if(appearance_input_.isNull())appearance_input_=cutout;seed_=seed;capture_=capture;original_=source;
  report_["seed"]=double(seed);report_["source_image"]=source;
  report_["preprocessing"]=QJsonObject{{"prepared_width",cutout.width()},{"prepared_height",cutout.height()},{"original_file_unchanged",true},{"pixel_art_sampling",crisp_}};
  report_["appearance_requested"]=appearance_enabled_;report_["source_texture_limit"]=texture_size_;
  report_["engine"]=config.engine();report_["models_directory"]=config.models_dir;
  if(!startGpuTelemetry()){fail("WDDM GPU memory telemetry is unavailable; generation was not started.");return false;}
  emit progress(1,"Verifying local runtime and existing Q4 weights");
  const auto cancel=cancelled_;const auto image_path=QDir(run_dir_).filePath("input.png");
  const auto health=std::make_shared<QJsonObject>();
  const auto texture=std::make_shared<QJsonObject>();
  disconnect(&prepare_,nullptr,this,nullptr);
  connect(&prepare_,&QFutureWatcher<QString>::finished,this,[this,image_path,health,texture]{
    if(!active_)return;
    report_["gpu_health"]=*health;
    // Hybrid texture: TRELLIS paints the whole object in the same run when its texture weights are installed.
    native_texture_=appearance_enabled_ && !appearance_input_.isNull() && texture->value("available").toBool();
    report_["native_texture"]=QJsonObject{{"requested",appearance_enabled_},{"available",texture->value("available")},
      {"reason",texture->value("reason")},{"used",native_texture_},
      {"stage_captures",native_texture_&&capture_?"skipped: the engine cannot capture stages while texturing":capture_?"saved":"off"}};
    QString error;
    try { error=prepare_.result(); } catch(const std::exception& e) { fail(QString::fromUtf8(e.what()));return; }
    if(!error.isEmpty()){fail(error);return;}
    if(available_ram()<generation_admission_ram()){fail("Generation preflight requires 3.25 GiB free physical RAM after dependency verification.");return;}
    report_["dependencies_sha256_verified"]=true;report_["engine_sha256"]=config_.engine_sha256;
    report_["post_sha256"]=config_.post_sha256;
    glb_=QDir(run_dir_).filePath("raw.glb");
    launch(Stage::Generate,config_.engine(),inferenceArguments(config_,image_path,glb_,seed_,capture_?QDir(run_dir_).filePath("stages"):QString(),native_texture_));
  });
  prepare_.setFuture(QtConcurrent::run([config,cancel,cutout,image_path,health,texture]{
    *health=runCudaHealth(config.runtime_dir);
    if(!health->value("ready").toBool())return health->value("message").toString();
    auto error=verify(config,cancel);if(!error.isEmpty())return error;
    *texture=textureWeights(config,cancel);
    if(cancel->load())return QString("Cancelled");
    if(!cutout.save(image_path,"PNG"))return QString("Could not save the reviewed cutout.");
    return QString();
  }));
  return true;
}

bool TrellisBackend::generateTexture(const BackendConfig& config,const QString& bundle,const QString& parent) {
  if(!begin(parent,true))return false;
  config_=config;texture_request_=true;appearance_enabled_=false;appearance_input_=QImage();
  report_["geometry_inference"]=false;report_["texture_only"]=true;
  report_["quality_acceptance"]="FAIL_PREVIOUS_VISUAL_TESTS";report_["experimental"]=true;
  emit progress(1,"Verifying saved texture session and Q4 dependencies");
  disconnect(&prepare_,nullptr,this,nullptr);auto cancel=cancelled_;
  auto verified=std::make_shared<QJsonObject>();auto health=std::make_shared<QJsonObject>();
  connect(&prepare_,&QFutureWatcher<QString>::finished,this,[this,verified,health]{
    if(!active_)return;
    QString error;try{error=prepare_.result();}catch(const std::exception& e){error=QString::fromUtf8(e.what());}
    report_["gpu_health"]=*health;
    if(!error.isEmpty()){fail(error);return;}
    texture_job_=*verified;report_["texture_session"]=texture_job_;report_["dependencies_sha256_verified"]=true;
    glb_=QDir(run_dir_).filePath("generated-pbr.glb");
    texture_args_={texture_job_["prepared"].toString(),texture_job_["models"].toString(),texture_job_["source"].toString(),
      QDir(texture_job_["prepared"].toString()).filePath("unchanged.ply"),QDir(run_dir_).filePath("texture.post"),
      QString::number(texture_job_["seed"].toInt()),QDir(run_dir_).filePath("stages")};
    auto args=texture_args_;args<<"--validate-only";
    launch(Stage::TextureValidate,texture_job_["worker"].toString(),args);
  });
  prepare_.setFuture(QtConcurrent::run([config,bundle,cancel,verified,health]{
    try{
      auto j=readJson(bundle);
      if(j["schema"].toInt()!=1 || j["resolution"].toInt()!=512 || j["atlas"].toInt()!=2048 || !j.contains("seed") || j["seed"].toInt(-1)<0)
        return QString("Unsupported saved texture session; requires explicit 512 geometry / 2048 atlas / seed.");
      const QDir base(QFileInfo(bundle).absolutePath());
      for(const auto& key:{"prepared","models","source","geometry","raw_ply","worker","baker"}){
        auto value=j[key].toString();if(value.isEmpty())return QString("Missing texture session field: ")+key;
        j[key]=QDir::isRelativePath(value)?base.absoluteFilePath(value):value;
      }
      const auto hashes=j["sha256"].toObject();
      QStringList required{j["source"].toString(),j["geometry"].toString(),j["raw_ply"].toString(),j["worker"].toString(),j["baker"].toString()};
      for(const auto& f:{"manifest.json","slat_normalized.bin","slat_coords.bin","dino_conditioning.bin","decoder_coords.bin","decoder_subdiv_0.bin","decoder_subdiv_1.bin","decoder_subdiv_2.bin","decoder_subdiv_3.bin","unchanged.ply"})required<<QDir(j["prepared"].toString()).filePath(f);
      for(const auto& f:{"tex_flow_512.gguf","tex_dec.gguf"})required<<QDir(j["models"].toString()).filePath(f);
      for(const auto& f:required){
        const auto expected=hashes[f].toString();
        if(expected.size()!=64 || sha256(f,cancel)!=expected)return QString("Saved texture session integrity mismatch: ")+f;
      }
      *health=runCudaHealth(config.runtime_dir);
      if(!health->value("ready").toBool())return health->value("message").toString();
      *verified=j;return QString();
    }catch(const std::exception& e){return QString::fromUtf8(e.what());}
  }));return true;
}

bool TrellisBackend::openSaved(const QString& glb,const QString& parent,bool reproject_source) {
  if(reproject_source && (!appearance_enabled_ || appearance_input_.isNull())){emit finished(false,"Explicit source reprojection requires an imported source image.");return false;}
  if(!QFileInfo(glb).isFile()){emit finished(false,"Choose an existing generated geometry GLB.");return false;}
  if(available_ram()<GiB+64*MiB){emit finished(false,"Saved preview preflight needs 1 GiB RAM reserve plus 64 MiB for bounded metadata inspection.");return false;}
  SavedPreviewMemory budget;
  try{budget=saved_preview_memory(native(glb));}
  catch(const std::exception& e){emit finished(false,QString::fromUtf8(e.what()));return false;}
  if(!begin(parent,false,budget.required_free_bytes))return false;
  preview_required_=budget.preview_free_bytes;
  config_={};original_=glb;glb_=glb;report_["saved_source"]=glb;reproject_source_=reproject_source;report_["source_reprojection_requested"]=reproject_source;report_["source_texture_limit"]=texture_size_;
  report_["memory_admission"]=QJsonObject{{"workload","saved_preview"},{"bounded_profile",budget.bounded_profile},
    {"required_free_bytes",double(budget.required_free_bytes)},{"preview_free_bytes",double(budget.preview_free_bytes)},
    {"encoded_bytes",double(budget.encoded_bytes)},{"decoded_image_bytes",double(budget.decoded_image_bytes)},
    {"vertices",double(budget.vertices)},{"triangles",double(budget.triangles)}};
  if(appearance_input_.isNull()){
    const auto source=QDir(QFileInfo(glb).absolutePath()).filePath("input.png");
    if(QFileInfo(source).isFile())appearance_input_=prepare_image(source).cutout;
  }
  if(!appearance_input_.isNull())appearance_input_.save(QDir(run_dir_).filePath("input.png"));
  validateOutput();return true;
}
bool TrellisBackend::resumeCleanup(const BackendConfig& config,const QString& raw_glb,const QString& parent){
  const auto ply=QDir(QFileInfo(raw_glb).absolutePath()).filePath("raw.ply");
  if(!QFileInfo(raw_glb).isFile() || !QFileInfo(ply).isFile()){emit finished(false,"Resume requires saved raw.glb and raw.ply from the same run.");return false;}
  if(!begin(parent,false))return false;
  config_=config;original_=raw_glb;glb_=QDir(run_dir_).filePath("geometry.glb");
  report_["resumed_from"]=raw_glb;report_["neural_inference_repeated"]=false;
  if(!QFile::copy(raw_glb,QDir(run_dir_).filePath("raw.glb"))){fail("Could not preserve the raw geometry in the resumed run.");return false;}
  if(appearance_input_.isNull())appearance_input_=prepare_image(QDir(QFileInfo(raw_glb).absolutePath()).filePath("input.png")).cutout;
  if(!appearance_input_.isNull())appearance_input_.save(QDir(run_dir_).filePath("input.png"));
  disconnect(&prepare_,nullptr,this,nullptr);const auto cancelled=cancelled_;
  connect(&prepare_,&QFutureWatcher<QString>::finished,this,[this,ply]{
    if(!active_)return;const auto error=prepare_.result();if(!error.isEmpty()){fail(error);return;}
    launch(Stage::Cleanup,config_.post_executable,{ply,glb_,"--preserve-topology"});
  });
  prepare_.setFuture(QtConcurrent::run([config,cancelled]{try{return sha256(config.post_executable,cancelled)==config.post_sha256?QString():QString("Pinned cleanup executable checksum mismatch");}catch(const std::exception& e){return QString::fromUtf8(e.what());}}));
  return true;
}
bool TrellisBackend::launch(Stage stage,const QString& executable,const QStringList& args) {
  if(!active_)return false;
  if(available_ram()<GiB){fail("The 1 GiB free physical RAM reserve is exhausted.");return false;}
  if(!QFileInfo(executable).isFile()){fail("Missing backend executable: "+executable);return false;}
  releaseJob();
#ifdef Q_OS_WIN
  HANDLE job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_JOB_MEMORY;
  limits.JobMemoryLimit=SIZE_T(6*GiB);
  if(!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){if(job)CloseHandle(job);fail("Could not set safe worker memory limits.");return false;}
  job_=job;
#else
  fail("The verified local TRELLIS backend requires Windows/CUDA.");return false;
#endif
  stage_=stage;phase_timer_.start();buffer_.clear();
  auto env=QProcessEnvironment::systemEnvironment();
  // Environment flags from earlier diagnostics must not change this request.
  for(const auto& name:env.keys())if(name.startsWith("TRELLIS_"))env.remove(name);
  for(const auto& name:{"OMP_NUM_THREADS","OPENBLAS_NUM_THREADS","MKL_NUM_THREADS"})env.insert(name,"2");
  env.insert("TRELLIS_DBG_MEM","1");env.insert("TRELLIS_DBG_ALLOC","1");
  if(stage==Stage::Generate && native_texture_){
    // Stop after the POST dump: raw mesh + PBR volume, before the engine's own remesh/UV/bake.
    env.insert("TRELLIS_DUMP_POST",QDir::toNativeSeparators(QDir(run_dir_).filePath("native.post")));
    env.insert("TRELLIS_STOP_AFTER_POST_DUMP","1");
    // Texture session (agent milestone 2, B2): the texture-candidates engine saves the texture stage's inputs so a
    // later texture-only replay can paint other candidates on this exact geometry. Ignored by older engines.
    env.insert("TRELLIS_TEX_SESSION",QDir::toNativeSeparators(QDir(run_dir_).filePath("texture.session")));
  }
  env.insert("PATH",config_.runtime_dir+";"+env.value("PATH"));
  process_.setProcessEnvironment(env);process_.setWorkingDirectory((stage==Stage::TextureValidate || stage==Stage::TextureGenerate)?config_.runtime_dir:run_dir_);
  emit logLine("Starting "+QFileInfo(executable).fileName());
  process_.start(executable,args);return true;
}
void TrellisBackend::readLogs() {
  const auto out=process_.readAllStandardOutput(),err=process_.readAllStandardError();
  stdout_.write(out);stderr_.write(err);stdout_.flush();stderr_.flush();
  auto text=out+err;text.replace('\r','\n');buffer_+=text;
  if(buffer_.size()>65536)buffer_=buffer_.right(65536);
  int end;
  while((end=buffer_.indexOf('\n'))>=0){
    const auto line=QString::fromUtf8(buffer_.left(end)).trimmed();buffer_.remove(0,end+1);
    if(line.isEmpty())continue;emit logLine(line.left(8192));
    if(stage_==Stage::Generate)observeInferenceLine(report_,line);
    if(stage_==Stage::TextureGenerate && line.contains("dit.forward") && line.contains("-> sched")){report_["inference_started"]=true;report_["generation_executed"]=true;}
    if(stage_==Stage::TextureGenerate && line.contains("[flow]"))emit progress(30,"Sampling native texture flow (experimental)");
    if(stage_==Stage::TextureGenerate && line.contains("[tex-dec"))emit progress(60,"Decoding native PBR fields");
    if(line.startsWith("[1/"))emit progress(5,"Preparing image");
    if(line.startsWith("[2/"))emit progress(8,"Image conditioning");
    if(line.startsWith("[3/")){flow_base_=10;emit progress(10,"Sparse structure");}
    if(line.startsWith("[4/")){flow_base_=35;emit progress(35,"Shape flow");}
    if(line.startsWith("[5/"))emit progress(60,"Shape decoding");
    if(line.startsWith("[6/") && stage_==Stage::Generate)emit progress(64,"TRELLIS texture of the whole object");
    const auto match=QRegularExpression("\\]\\s+(\\d+)/12").match(line);
    if(stage_==Stage::Generate && line.contains("[flow]") && match.hasMatch())emit progress(flow_base_+match.captured(1).toInt()*2,"Sampling shape");
    if(line.contains("[post-time]"))emit progress(78,"Topology-preserving cleanup");
  }
}
void TrellisBackend::processDone(int code,QProcess::ExitStatus status) {
  readLogs();sampleResources();if(!active_){stdout_.close();stderr_.close();telemetry_.close();return;}
  const QString name=stage_==Stage::TextureValidate?"texture_preflight":stage_==Stage::TextureGenerate?"texture_inference":stage_==Stage::TextureBake?"texture_bake":stage_==Stage::Generate?"generation":stage_==Stage::Cleanup?"cleanup":stage_==Stage::Inspect?"inspection":stage_==Stage::AutoTexture?"auto_texture":"intersections";
  phases_[name]=phase_timer_.elapsed()/1000.;releaseJob();
  // Auto Texture never fails the run: any outcome falls back to the best available appearance.
  if(stage_==Stage::AutoTexture){finishAutoTexture(code,status!=QProcess::NormalExit);return;}
  if(stage_==Stage::Generate && native_texture_ && (status!=QProcess::NormalExit || code || !QFileInfo(QDir(run_dir_).filePath("native.post")).isFile())){
    retryGeometryOnly(QString("TRELLIS texture run failed (exit %1)").arg(code));return;
  }
  if(status!=QProcess::NormalExit || code){
    auto message=QString("%1 failed (exit %2). Original outputs and logs were preserved.").arg(name).arg(code);
    if(stage_==Stage::Generate){finishInference(report_,false,false);const auto cuda=report_.value("cuda_runtime_diagnostic").toString();if(!cuda.isEmpty())message=cuda+". CUDA device discovery failed; this does not mean the backend is missing. "+message;}
    if(stage_==Stage::Cleanup && QFileInfo(QDir(run_dir_).filePath("raw.glb")).isFile()){
      report_["cleanup_failed"]=true;report_["cleanup_error"]=message;
      QFile log(QDir(run_dir_).filePath("stderr.log"));if(log.open(QIODevice::ReadOnly)){log.seek(std::max<qint64>(0,log.size()-4096));report_["cleanup_diagnostic"]=QString::fromUtf8(log.readAll());}
      emit logLine(message+" Previewing and validating raw geometry; inference will not repeat.");
      glb_=QDir(run_dir_).filePath("raw.glb");validateOutput();return;
    }
    if(stage_==Stage::Intersections){report_["audit_error"]=message;finishInspection();return;}
    fail(message);return;
  }
  if(stage_==Stage::TextureValidate){
    if(available_ram()<generation_admission_ram()){fail("Texture generation requires 3.25 GiB free physical RAM after validation.");return;}
    if(!startGpuTelemetry()){fail("WDDM GPU telemetry unavailable; texture inference not started.");return;}
    emit progress(5,"Native Q4 texture generation · experimental");
    launch(Stage::TextureGenerate,texture_job_["worker"].toString(),texture_args_);
  }else if(stage_==Stage::TextureGenerate){
    report_["inference_completed"]=true;report_["inference_started"]=true;
    report_["generation_executed"]=true;stopGpuTelemetry();telemetry_failures_=0;
    emit progress(75,"Baking 2048 Base Color and Metallic-Roughness");
    launch(Stage::TextureBake,texture_job_["baker"].toString(),{QDir(run_dir_).filePath("texture.post"),texture_job_["raw_ply"].toString(),glb_,"2048"});
  }else if(stage_==Stage::TextureBake){
    try{
      const auto bake=readJson(glb_+".bake.json");report_["texture_validation"]=bake;
      if(!bake["oriented_triangle_multiset_exact"].toBool() || bake["atlas_size"].toInt()!=2048)throw std::runtime_error("Texture bake did not preserve geometry or 2048 atlas contract");
      const auto info=material_info(native(glb_));const auto maps=info.at("maps");
      for(const auto* name:{"base_color","metallic_roughness"})if(maps.at(name).at("size")!=nlohmann::json::array({2048,2048}))throw std::runtime_error("Generated PBR map missing or wrong size");
      if(!geometrically_identical(load_geometry_glb(native(glb_)),load_geometry_glb(native(texture_job_["geometry"].toString()))))throw std::runtime_error("Texturing changed the saved geometry");
      report_["pbr_maps_valid"]=true;report_["uv_complete"]=bake["uv_zero_area_triangles"].toInt(-1)==0;
      validateOutput();
    }catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
  }else if(stage_==Stage::Generate){
    if(native_texture_){
      // The dump's raw mesh is the geometry-only run's mesh (same seed, same stages): write the same raw files.
      try{
        const auto split=split_native_post(native(QDir(run_dir_).filePath("native.post")),native(QDir(run_dir_).filePath("raw.ply")),native(glb_),seed_);
        auto info=report_["native_texture"].toObject();
        info["dump_bytes"]=double(QFileInfo(QDir(run_dir_).filePath("native.post")).size());info["pbr_voxels"]=double(split.voxels);
        info["resolution"]=split.resolution;info["raw_vertices"]=double(split.vertices);info["raw_triangles"]=double(split.triangles);
        report_["native_texture"]=info;
      }catch(const std::exception& e){retryGeometryOnly(QString("Could not read the TRELLIS texture dump: ")+e.what());return;}
    }
    if(!QFileInfo(glb_).isFile() || !QFileInfo(QDir(run_dir_).filePath("raw.ply")).isFile()){fail("Generation did not produce its expected geometry outputs.");return;}
    finishInference(report_,true,true);
    glb_=QDir(run_dir_).filePath("geometry.glb");emit progress(72,"Topology-preserving cleanup");
    launch(Stage::Cleanup,config_.post_executable,{QDir(run_dir_).filePath("raw.ply"),glb_,"--preserve-topology"});
  }else if(stage_==Stage::Cleanup)validateOutput();
  else if(stage_==Stage::Inspect){
    emit progress(92,"Checking actual surface intersections");
    launch(Stage::Intersections,helper("pixel3d-geometry-audit"),
           {QDir(run_dir_).filePath("inspection/geometry.bin"),QDir(run_dir_).filePath("inspection/intersections.json")});
  }else if(stage_==Stage::Intersections)finishInspection();
}
void TrellisBackend::retryGeometryOnly(const QString& reason){
  auto info=report_["native_texture"].toObject();
  info["used"]=false;info["failed"]=true;info["error"]=reason;report_["native_texture"]=info;
  report_["native_texture_failed"]=true;
  for(const auto& f:{QString("native.post"),QString("raw.ply"),QString("raw.glb")})QFile::remove(QDir(run_dir_).filePath(f));
  native_texture_=false;
  emit logLine(reason+" — generating geometry only (texture falls back to the picture-only reconstruction).");
  emit progress(4,"Retrying geometry only");
  glb_=QDir(run_dir_).filePath("raw.glb");
  launch(Stage::Generate,config_.engine(),inferenceArguments(config_,QDir(run_dir_).filePath("input.png"),glb_,seed_,capture_?QDir(run_dir_).filePath("stages"):QString()));
}
void TrellisBackend::validateOutput() {
  emit progress(88,"Checking exported topology");
  launch(Stage::Inspect,helper("pixel3d-asset-inspect"),{glb_,QDir(run_dir_).filePath("inspection")});
}
void TrellisBackend::finishInspection() {
  try {
    const auto topology=readJson(QDir(run_dir_).filePath("inspection/topology.json"));
    const auto cleanupReport=QDir(run_dir_).filePath("geometry.glb.cleanup.json");
    if(QFileInfo::exists(cleanupReport))report_["cleanup_repairs"]=readJson(cleanupReport);
    QJsonObject crossings;
    try{crossings=readJson(QDir(run_dir_).filePath("inspection/intersections.json"));}
    catch(...){crossings=QJsonObject{{"complete",false},{"vertices",topology.value("vertices")},{"faces",topology.value("triangles")},{"pair_classification",QJsonObject{}},{"edge_star_classification",QJsonObject{}}};}
    asset_.validation=classify_validation(nlohmann::json::parse(QJsonDocument(topology).toJson().constData()),
                                          nlohmann::json::parse(QJsonDocument(crossings).toJson().constData()));
    report_["topology"]=topology;
    // Detailed pair examples stay on disk; keep the UI report bounded.
    auto summary=crossings;summary.remove("examples");summary.remove("edge_stars");summary.remove("component_records");
    report_["intersections"]=summary;report_["geometry_glb"]=glb_;
    report_["limitations"]=QJsonArray{"Material origin is reported separately. The native preview displays base color only; export retains supported embedded PBR maps.",
      "Q4 512 results can contain slabs, disconnected surfaces and self-intersections.",
      "A completed generation is not proof of safe topology; unknown/incomplete audits never pass.",
      "Preview centering/scaling is display-only; GLB coordinates are preserved."};
    startAppearance();
  }catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
}
void TrellisBackend::startAppearance(){
  // Auto Texture runs when there is a picture and no material to keep: every new generation, a saved
  // geometry-only GLB, or an explicit "apply the picture again" on a saved result.
  bool saved_material=false;
  if(!reproject_source_)try{saved_material=material_info(native(glb_)).value("kind",std::string("geometry_only"))!="geometry_only";}catch(...){}
  if(texture_request_ || saved_material || !appearance_enabled_ || appearance_input_.isNull()){preparePreview();return;}
  const auto worker=helper("pixel3d-auto-texture");
  if(!QFileInfo(worker).isFile()){
    report_["auto_texture"]=QJsonObject{{"status","UNAVAILABLE"},{"message","Auto Texture worker missing; approximate projection used."}};
    preparePreview();return;
  }
  const auto source=QDir(run_dir_).filePath("texture-source.png");
  if(!QFileInfo::exists(source) && !appearance_input_.save(source,"PNG")){
    report_["auto_texture"]=QJsonObject{{"status","ERROR"},{"message","Could not save the picture for Auto Texture."}};
    preparePreview();return;
  }
  textured_=QDir(run_dir_).filePath("textured.glb");
  QStringList args{glb_,source,textured_,"--max-size",texture_size_>=1024?"4096":"1024","--seconds","300"};
  if(crisp_)args<<"--crisp";
  // TRELLIS's own texture of this geometry (same run folder): the hybrid's base layer.
  const auto native_post=QDir(QFileInfo(glb_).absolutePath()).filePath("native.post");
  if(QFileInfo(native_post).isFile())args<<"--native"<<native_post;
  // Other angles of the same object: saved into the run (the run stays reproducible) and passed with their labels.
  QJsonArray views;
  for(int k=0;k<extra_pictures_.size() && k<5;++k){
    const auto& e=extra_pictures_[k];
    const auto file=QDir(run_dir_).filePath(QString("view-%1-%2.png").arg(k+1).arg(QString(e.label).replace(':','_')));
    if(e.image.isNull() || !e.image.save(file,"PNG"))continue;
    args<<"--view"<<file<<e.label;
    views.append(QJsonObject{{"file",file},{"label",e.label},{"name",e.name}});
  }
  if(!views.isEmpty())report_["extra_pictures"]=views;
  emit progress(93,QFileInfo(native_post).isFile()?"Hybrid Texture: the picture on the front, TRELLIS's texture everywhere else"
                                                  :"Auto Texture: matching the picture to the model and baking textures");
  launch(Stage::AutoTexture,worker,args);
}
void TrellisBackend::finishAutoTexture(int code,bool crashed){
  QJsonObject summary;
  try{summary=readJson(textured_+".texture.json");}catch(...){}
  const auto selected=summary.value("selected").toString();
  QJsonObject quality;
  for(const auto& c:summary.value("candidates").toArray())if(c.toObject().value("name").toString()==selected)quality=c.toObject().value("quality").toObject();
  quality.remove("issues");
  QJsonObject bake;
  for(const auto& a:summary.value("attempts").toArray()){
    const auto b=a.toObject().value("bake").toObject();
    if(!b.isEmpty() && b.value("atlas_size")==quality.value("texture_size").toArray().first())bake=b;
  }
  const auto reg=summary.value("registration").toObject();
  report_["auto_texture"]=QJsonObject{{"status",crashed?QString("CRASHED"):summary.value("status").toString("ERROR")},{"exit_code",code},
    {"selected",selected},{"verdict",summary.value("verdict")},{"issues",summary.value("issues")},{"limitations",summary.value("limitations")},
    {"error",summary.value("error")},{"registration",QJsonObject{{"yaw",reg.value("yaw")},{"elevation",reg.value("elevation")},{"iou",reg.value("iou")},
      {"ambiguous",reg.value("ambiguous")},{"silhouette_usable",reg.value("silhouette_usable")},{"choice",reg.value("choice")}}},
    {"observed_fraction",bake.value("observed_fraction")},{"blended_fraction",bake.value("blended_fraction")},
    {"native_fraction",bake.value("native_fraction")},{"native",bake.value("native")},
    {"views",summary.value("views")},{"picture_ownership",bake.value("picture_ownership")},{"pictures",bake.value("pictures")},
    {"quality",quality},{"seconds",summary.value("seconds")},{"peak_working_set_mib",summary.value("peak_working_set_mib")},
    {"report",textured_+".texture.json"}};
  if(crashed || code!=0 || !QFileInfo(textured_).isFile()){
    if(code!=3)report_["auto_texture_failed"]=true;  // 3 = the GLB's own material scored better and is kept
    emit logLine(code==3?"Auto Texture kept the saved material (better quality checks).":"Auto Texture did not produce a texture; using the best available fallback.");
    textured_.clear();
  }
  preparePreview();
}
void TrellisBackend::preparePreview(){
  try{
    const auto file=native(QDir(run_dir_).filePath("inspection/geometry.bin"));
    if(available_ram()<preview_required_)throw std::runtime_error("Preview stopped: insufficient RAM for the bounded preview allocations and 1 GiB reserve.");
    emit progress(97,"Loading interactive preview");
    const auto source=appearance_input_;const auto output=QDir(run_dir_).filePath("appearance.glb");
    const auto enabled=appearance_enabled_;const auto size=texture_size_;const auto crisp=crisp_;const auto saved=glb_;
    const auto required=preview_required_;const auto reproject=reproject_source_;const auto textured=textured_;
    preview_.setFuture(QtConcurrent::run([file,source,output,enabled,size,crisp,saved,required,reproject,textured]{
      if(available_ram()<required)throw std::runtime_error("Preview stopped: available memory changed before allocation.");
      PreparedAsset result;result.geometry=std::make_shared<const pixel3d::Mesh>(read_geometry_binary(file));
      // Auto Texture output (already re-imported and checked by the worker) comes first.
      if(!textured.isEmpty())try{
        result.appearance=load_generated_appearance(native(textured));
        if(result.appearance){result.materials=material_info(native(textured));result.textured_glb=textured;return result;}
      }catch(const std::exception& e){result.material_error=QString::fromUtf8(e.what());result.appearance.reset();}
      // A saved material takes precedence over any sibling/imported image.
      // Only explicit source reapplication can create a new projected asset;
      // the saved UV/PBR source file is never overwritten.
      try{if(!reproject)result.appearance=load_generated_appearance(native(saved));
        if(result.appearance){result.materials=material_info(native(saved));return result;}}
      catch(const std::exception& e){result.material_error=QString::fromUtf8(e.what());return result;}
      if(enabled && !source.isNull())try{result.appearance=project_source_material(*result.geometry,source,size,crisp);save_source_material(*result.appearance,output);result.textured_glb=output;}
      catch(const std::exception& e){result.material_error=QString::fromUtf8(e.what());result.appearance.reset();}
      if(result.appearance)result.materials={{"mode","approximate_single_image_projection"},{"kind","source_projected"},{"pbr_validated",false}};
      return result;
    }));
  }catch(const std::exception& e){fail(QString::fromUtf8(e.what()));}
}
bool TrellisBackend::exportGlb(const QString& destination,bool acknowledged,QString& error)const {
  if(active_){error="Wait for the current operation to finish.";return false;}
  return exportAsset(asset_,destination,acknowledged,error);
}
bool TrellisBackend::exportAsset(const MeshAsset& asset,const QString& destination,bool acknowledged,QString& error){
  if(!asset.geometry || !QFileInfo(path(asset.geometry_glb)).isFile()){error="No completed mesh is available.";return false;}
  if(asset.validation!=Validation::Pass && !acknowledged){error="Review and acknowledge the reported geometry defects before export.";return false;}
  if(QFileInfo::exists(destination) || QFileInfo::exists(destination+".validation.json")){error="Choose a new filename; existing assets are never overwritten.";return false;}
  QFile validation(path(asset.report));
  if(!validation.open(QIODevice::ReadOnly) || validation.size()>32*1024*1024){error="The saved validation report is unavailable.";return false;}
  const auto validation_bytes=validation.readAll();
  QFile input(path(asset.geometry_glb));QSaveFile output(destination);output.setDirectWriteFallback(false);
  if(!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)){error="Could not open the export location.";return false;}
  QCryptographicHash original(QCryptographicHash::Sha256),copied(QCryptographicHash::Sha256);
  while(!input.atEnd()){
    const auto bytes=input.read(1024*1024);if(bytes.isEmpty() && input.error()!=QFileDevice::NoError){error="Source read failed.";return false;}
    original.addData(bytes);if(output.write(bytes)!=bytes.size()){error="Export write failed.";return false;}copied.addData(bytes);
  }
  if(original.result()!=copied.result() || !output.commit()){error="Export integrity/commit failed.";return false;}
  try{const auto reloaded=load_geometry_glb(native(destination));
    if(!geometrically_identical(reloaded,*asset.geometry)){error="Export round-trip geometry differs; the export is not validated.";return false;}}
  catch(const std::exception& e){error="Export round-trip failed: "+QString::fromUtf8(e.what());return false;}
  QSaveFile sidecar(destination+".validation.json");sidecar.setDirectWriteFallback(false);
  if(!sidecar.open(QIODevice::WriteOnly) || sidecar.write(validation_bytes)!=validation_bytes.size() || !sidecar.commit()){
    error="GLB was saved, but its validation sidecar could not be committed; retain the original run report.";return false;
  }
  return true;
}
void TrellisBackend::cancel(){if(active_)fail("Cancelled. Original assets and partial diagnostic outputs were preserved.");}
void TrellisBackend::fail(const QString& message){
  if(!active_)return;
  active_=false;if(cancelled_)cancelled_->store(true);monitor_.stop();releaseJob();stopGpuTelemetry();
  if(process_.state()!=QProcess::NotRunning)process_.kill();
  report_["status"]="ERROR";report_["message"]=message;report_["runtime_seconds"]=timer_.elapsed()/1000.;report_["phases"]=phases_;
  saveReport();
  if(process_.state()==QProcess::NotRunning){stdout_.close();stderr_.close();telemetry_.close();}
  emit finished(false,message);
}
bool TrellisBackend::saveReport(){
  report_["memory"]=QJsonObject{{"sampled_peak_rss_bytes",double(peak_rss_)},{"peak_job_commit_bytes",double(peak_commit_)},
    {"minimum_free_physical_bytes",double(minimum_free_)},{"sampled_peak_pid_dedicated_bytes",double(peak_gpu_)},
    {"sampled_peak_pid_shared_bytes",double(peak_shared_)},{"gpu_measured",gpu_samples_>0},{"valid_gpu_samples",gpu_samples_},
    {"gpu_note","WDDM PID samples at 1-second intervals; short peaks may be missed. Internal managed VRAM cap is 3584 MiB."}};
  QSaveFile out(QDir(run_dir_).filePath("report.json"));const auto data=QJsonDocument(report_).toJson();
  return out.open(QIODevice::WriteOnly) && out.write(data)==data.size() && out.commit();
}
void TrellisBackend::releaseJob(){
#ifdef Q_OS_WIN
  if(job_)CloseHandle(static_cast<HANDLE>(job_));
#endif
  job_=nullptr;
}
bool TrellisBackend::containAndResume(){
#ifdef Q_OS_WIN
  if(!job_)return false;const auto pid=DWORD(process_.processId());
  HANDLE child=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!child)return false;const bool assigned=AssignProcessToJobObject(static_cast<HANDLE>(job_),child)!=FALSE;CloseHandle(child);
  if(!assigned)return false;
  HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
  THREADENTRY32 entry{};entry.dwSize=sizeof(entry);bool resumed=false;
  if(Thread32First(snapshot,&entry))do{
    if(entry.th32OwnerProcessID!=pid)continue;
    HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME,FALSE,entry.th32ThreadID);
    if(thread){resumed=ResumeThread(thread)!=DWORD(-1);CloseHandle(thread);}break;
  }while(Thread32Next(snapshot,&entry));CloseHandle(snapshot);return resumed;
#else
  return false;
#endif
}
bool TrellisBackend::startGpuTelemetry(){
#ifdef Q_OS_WIN
  PDH_HQUERY query=nullptr;PDH_HCOUNTER a=nullptr,b=nullptr;
  if(PdhOpenQueryW(nullptr,0,&query)!=ERROR_SUCCESS)return false;
  if(PdhAddEnglishCounterW(query,L"\\GPU Process Memory(*)\\Dedicated Usage",0,&a)!=ERROR_SUCCESS ||
     PdhAddEnglishCounterW(query,L"\\GPU Process Memory(*)\\Shared Usage",0,&b)!=ERROR_SUCCESS){PdhCloseQuery(query);return false;}
  gpu_query_=query;gpu_dedicated_=a;gpu_shared_=b;PdhCollectQueryData(query);return true;
#else
  return false;
#endif
}
void TrellisBackend::stopGpuTelemetry(){
#ifdef Q_OS_WIN
  if(gpu_query_)PdhCloseQuery(static_cast<PDH_HQUERY>(gpu_query_));
#endif
  gpu_query_=gpu_dedicated_=gpu_shared_=nullptr;
}
void TrellisBackend::sampleResources(){
  if(!active_)return;
  const auto free=available_ram();minimum_free_=std::min(minimum_free_,free);quint64 rss=0,dedicated=0,shared=0;bool current_gpu_measured=false;
#ifdef Q_OS_WIN
  if(process_.state()==QProcess::Running){
    HANDLE child=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,DWORD(process_.processId()));
    if(child){PROCESS_MEMORY_COUNTERS counters{};counters.cb=sizeof(counters);if(GetProcessMemoryInfo(child,&counters,sizeof(counters)))rss=counters.WorkingSetSize;CloseHandle(child);}
  }
  if(job_){JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if(QueryInformationJobObject(static_cast<HANDLE>(job_),JobObjectExtendedLimitInformation,&info,sizeof(info),nullptr))peak_commit_=std::max(peak_commit_,quint64(info.PeakJobMemoryUsed));}
  if(gpu_query_ && (stage_==Stage::Generate || stage_==Stage::TextureGenerate) && process_.state()==QProcess::Running){
    bool ok=PdhCollectQueryData(static_cast<PDH_HQUERY>(gpu_query_))==ERROR_SUCCESS;
    auto read=[&](void* counter,quint64& sum,bool& found){
      DWORD bytes=0,count=0;auto status=PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter),PDH_FMT_LARGE,&bytes,&count,nullptr);
      if(status!=PDH_MORE_DATA)return false;
      std::vector<unsigned char> buffer(bytes);auto items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
      if(PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(counter),PDH_FMT_LARGE,&bytes,&count,items)!=ERROR_SUCCESS)return false;
      const auto prefix=QString("pid_%1_").arg(process_.processId());
      for(DWORD i=0;i<count;++i)
        found=collectGpuValue(QString::fromWCharArray(items[i].szName),prefix,
          items[i].FmtValue.CStatus==PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus==PDH_CSTATUS_NEW_DATA,
          items[i].FmtValue.largeValue,sum)||found;
      return true;
    };
    bool dedicated_found=false,shared_found=false;
    const bool a=read(gpu_dedicated_,dedicated,dedicated_found),b=read(gpu_shared_,shared,shared_found);
    const bool api_ok=ok&&a&&b;
    ok=observedGpuSample(api_ok,dedicated_found,shared_found);
    // No GPU PID exists during CPU-only model preparation. API failures still
    // stop after three samples; missing PID after compute starts does too.
    const bool failed=!api_ok || (report_.value("inference_started").toBool()&&!ok);
    telemetry_failures_=failed?telemetry_failures_+1:0;
    if(ok)++gpu_samples_;current_gpu_measured=ok;
  }
#endif
  emit resources(rss/double(MiB),dedicated/double(MiB),current_gpu_measured);
  peak_rss_=std::max(peak_rss_,rss);peak_gpu_=std::max(peak_gpu_,dedicated);peak_shared_=std::max(peak_shared_,shared);
  telemetry_.write(QString("%1,%2,%3,%4,%5,%6\n").arg(timer_.elapsed()).arg(rss).arg(peak_commit_).arg(free).arg(dedicated).arg(shared).toUtf8());telemetry_.flush();
  if(free<GiB)fail("Stopped: the 1 GiB free physical RAM reserve was exhausted.");
  else if(dedicated>3900*MiB || shared>512*MiB)fail("Stopped: the existing GPU memory safety limit was exceeded.");
  else if(telemetry_failures_>=3)fail("Stopped: GPU memory telemetry failed for three samples.");
  else if(timer_.elapsed()>1200000)fail("Stopped: the 20-minute bounded runtime was exceeded.");
  else if(stage_==Stage::Intersections && phase_timer_.elapsed()>65000)fail("Intersection validation timed out; the mesh is not certified safe.");
  else if(stage_==Stage::AutoTexture && phase_timer_.elapsed()>480000 && process_.state()!=QProcess::NotRunning){
    report_["auto_texture_timeout"]=true;emit logLine("Auto Texture exceeded 8 minutes; stopping it and keeping the best available appearance.");process_.kill();}
  else if(QStorageInfo(run_dir_).bytesAvailable()<qint64(GiB))fail("Stopped: disk reserve fell below 1 GiB.");
}
}  // namespace p3dasset
