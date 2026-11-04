#include "trellis_diagnostics.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLibrary>
#include <QProcess>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
int main(int argc,char** argv){
  QCoreApplication app(argc,argv);const auto args=app.arguments();
  auto output=[&](QJsonObject j){j["schema"]=1;const auto b=QJsonDocument(j).toJson(QJsonDocument::Compact);std::puts(b.constData());return 0;};
  if(args.size()!=3 || args[1]!="--runtime")return output({{"ready",false},{"status","invalid_arguments"},{"message","Expected --runtime directory"}});
  QProcess smi;
#ifdef Q_OS_WIN
  smi.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a){a->flags|=CREATE_NO_WINDOW;});
#endif
  smi.start(QDir(qEnvironmentVariable("SystemRoot","C:/Windows")).filePath("System32/nvidia-smi.exe"),{"--query-gpu=name","--format=csv,noheader"});
  const bool started=smi.waitForStarted(1000);const bool ended=started&&smi.waitForFinished(3000);
  if(!ended){smi.kill();smi.waitForFinished(500);}
  const auto health=QString::fromLocal8Bit(smi.readAllStandardOutput()+smi.readAllStandardError()).trimmed();
  if(!started)return output({{"ready",false},{"status","environment_probe_unavailable"},{"message","NVIDIA health query could not start; GPU readiness is unknown."},{"runtime_tested",false}});
  if(!ended)return output({{"ready",false},{"status","environment_probe_timeout"},{"message","NVIDIA health query timed out; GPU readiness is unknown."},{"runtime_tested",false}});
  if(!ended || smi.exitStatus()!=QProcess::NormalExit || smi.exitCode()!=0 || health.isEmpty()){
    auto j=p3dasset::cudaDiagnosis(false,false,0,{},{},0);j["environment_diagnostic"]=health;
    j["message"]=j.value("message").toString()+" "+health;j["runtime_tested"]=false;return output(j);
  }
  const QDir runtime(args[2]);
#ifdef Q_OS_WIN
  SetDllDirectoryW(reinterpret_cast<LPCWSTR>(runtime.absolutePath().utf16()));
#endif
  QLibrary cuda(runtime.filePath("cudart64_12.dll"));
  using Count=int(*)(int*);using Error=const char*(*)(int);
  auto count=reinterpret_cast<Count>(cuda.resolve("cudaGetDeviceCount"));
  auto name=reinterpret_cast<Error>(cuda.resolve("cudaGetErrorName"));
  auto message=reinterpret_cast<Error>(cuda.resolve("cudaGetErrorString"));
  if(!count || !name || !message)return output({{"ready",false},{"status","runtime_dependency_missing"},{"message",cuda.errorString()}});
  int devices=0;const int code=count(&devices);
  if(code){auto j=p3dasset::cudaDiagnosis(true,true,code,QString::fromLatin1(name(code)),QString::fromLatin1(message(code)),devices);j["backend_registration_checked"]=false;return output(j);}
  QLibrary base(runtime.filePath("ggml-base.dll"));QLibrary ggml(runtime.filePath("ggml.dll"));
  auto resolve=[&](const char* symbol){auto f=base.resolve(symbol);return f?f:ggml.resolve(symbol);};
  using Find=void*(*)(const char*);using Devices=size_t(*)(void*);
  auto find=reinterpret_cast<Find>(resolve("ggml_backend_reg_by_name"));
  auto n=reinterpret_cast<Devices>(resolve("ggml_backend_reg_dev_count"));
  if(!find||!n)return output({{"ready",false},{"status","backend_dependency_missing"},{"message","GGML registration API unavailable in selected runtime."}});
  void* reg=find("CUDA");auto j=p3dasset::cudaDiagnosis(true,reg!=nullptr,code,QString::fromLatin1(name(code)),QString::fromLatin1(message(code)),reg?int(n(reg)):0);
  j["runtime_device_count"]=devices;j["backend_registration_checked"]=true;j["runtime_tested"]=true;return output(j);
}
