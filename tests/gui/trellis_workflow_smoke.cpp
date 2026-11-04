// Drives the real native window/controller; saved replay never claims inference.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include "studio/image3d_workspace.hpp"
#include "studio/studio_window.hpp"
#include "main_window.hpp"
#include "viewport.hpp"

int main(int argc,char** argv){
  bool native=false;for(int i=1;i<argc;++i)native|=QString::fromLocal8Bit(argv[i])=="--native-windows";
  qputenv("QT_QPA_PLATFORM",native?"windows":"offscreen");QApplication app(argc,argv);QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
  QCommandLineParser cli;cli.addHelpOption();cli.addOptions({{"source-image","Original source for HD reprojection","file"},{"mode","saved or generate","mode"},{"input","Existing fixture","file"},
    {"config","Local backend config","file"},{"results","New evidence folder","directory"},{"expect-validation","Optional expected validation status","status"},{"material-views","Five unlit material comparison views"},{"native-windows","Use actual Windows GUI and normal preview renderer"}});cli.process(app);
  const auto mode=cli.value("mode"),input=cli.value("input"),out=cli.value("results");
  if((mode!="saved" && mode!="generate" && mode!="resume" && mode!="source-hd" && mode!="texture") || !QFileInfo(input).isFile() || out.isEmpty() || QFileInfo::exists(out))return 4;
  if(!QDir().mkpath(out))return 5;
  p3dgui::WindowOptions options;options.software_view=!native;p3dgui::StudioWindow window(options);window.show();window.pixelBuilder().runAction("image3d");
  auto* dialog=&window.imageWorkspace();if(window.currentPage()!="image")return 6;dialog->showStep(p3dgui::Image3DWorkspace::Generate);
  dialog->findChild<QLineEdit*>("image3dOutput")->setText(out);
  dialog->findChild<QLineEdit*>("image3dSetup")->setText(cli.value("config"));
  bool completed=false;bool hdApplied=false;int beats=0;
  QTimer heartbeat;heartbeat.setInterval(20);QObject::connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start();
  QJsonObject summary{{"mode",mode},{"neural_generation_requested",mode=="generate"||mode=="texture"},{"geometry_generation_requested",mode=="generate"},{"input",input},{"native_windows_gui",native}};
  auto finish=[&](int code){
    summary["exit_code"]=code;summary["heartbeat_ticks"]=beats;summary["backend_report"]=dialog->backend().report();
    summary["run_directory"]=dialog->backend().runDirectory();summary["status_text"]=dialog->statusText();
    QSaveFile file(QDir(out).filePath("workflow-summary.json"));if(file.open(QIODevice::WriteOnly)){file.write(QJsonDocument(summary).toJson());if(!file.commit())code=7;}else code=7;
    window.pixelBuilder().controller().cleanShutdown();app.exit(code);
  };
  QObject::connect(&dialog->backend(),&p3dasset::TrellisBackend::finished,&window,[&](bool available,const QString& message){
    if(mode=="source-hd"&&!hdApplied&&available){
      hdApplied=true;auto* ready=new QTimer(&window);ready->setInterval(100);auto started=std::make_shared<bool>(false);
      QObject::connect(ready,&QTimer::timeout,&window,[&,ready,started]{
        if(dialog->property("image3dUiBusy").toBool())return;
        if(!*started){*started=true;dialog->importImage(cli.value("source-image"));return;}
        auto* button=dialog->findChild<QPushButton*>("image3dApplySource");if(!button||!button->isEnabled()){ready->stop();summary["hd_button_unavailable"]=true;finish(14);return;}
        ready->stop();dialog->findChild<QComboBox*>("image3dAppearanceMode")->setCurrentIndex(3);button->click();
      });ready->start();return;
    }
    if(completed)return;completed=true;summary["output_available"]=available;summary["message"]=message;
    if(!available){finish(2);return;}
    auto* wait=new QTimer(&window);wait->setInterval(100);
    QObject::connect(wait,&QTimer::timeout,&window,[&,wait]{
      if(dialog->preview().scene().empty())return;wait->stop();
      const auto asset=dialog->backend().asset();const auto name=QString::fromLatin1(p3dasset::validation_name(asset.validation));summary["geometry_validation"]=name;
      summary["colored_preview"]=dialog->preview().scene().textured;
      summary["texture_available"]=bool(asset.appearance && asset.appearance->texture);
      if(!cli.value("expect-validation").isEmpty() && cli.value("expect-validation")!=name){summary["unexpected_validation"]=true;finish(3);return;}
      QString error;const auto exported=QDir(out).filePath("Exported.glb");
      // Deliberate diagnostic export: unsafe status is retained in the report.
      if(!p3dasset::TrellisBackend::exportAsset(asset,exported,true,error)){summary["export_error"]=error;finish(8);return;}
      try{
        const auto reloaded=p3dasset::load_geometry_glb(std::filesystem::path(exported.toStdWString()));
        const bool same=p3dasset::geometrically_identical(reloaded,*asset.geometry);
        summary["export_reimport_geometry_identical"]=same;summary["vertices"]=double(reloaded.positions.size());summary["triangles"]=double(reloaded.tris.size());
        const auto appearance=p3dasset::load_generated_appearance(std::filesystem::path(exported.toStdWString()));
        summary["export_embedded_texture_reimported"]=bool(appearance && appearance->texture);
        if(asset.appearance && (!appearance || !(*appearance->texture==*asset.appearance->texture))){finish(13);return;}
        if(!same){finish(9);return;}
      }catch(const std::exception& e){summary["export_error"]=QString::fromUtf8(e.what());finish(10);return;}
      auto* views=new QTimer(&window);views->setInterval(300);auto index=std::make_shared<int>(0);
      QObject::connect(views,&QTimer::timeout,&window,[&,views,index]{
        const int n=*index;
        if(n){dialog->preview().grab().save(QDir(out).filePath(QString("view-%1.png").arg(n-1)));}
        const bool material=cli.isSet("material-views");
        if(n==(material?5:4)){views->stop();dialog->grab().save(QDir(out).filePath("NativeWorkflow.png"));summary["camera_views"]=material?5:4;finish(0);return;}
        if(material){
          auto& state=dialog->preview().state();state.lit=false;state.grid=false;state.overlay=false;state.yaw=n==1?180.f:n==2?90.f:n==3?270.f:0.f;state.pitch=0;state.ortho=true;state.preset.reset();
          if(n==4)dialog->preview().setIso();dialog->preview().refresh();++*index;return;
        }
        if(n==0)dialog->preview().setPreset(pixel3d::ViewRole::Front);
        if(n==1)dialog->preview().setPreset(pixel3d::ViewRole::Side);
        if(n==2)dialog->preview().setPreset(pixel3d::ViewRole::Top);
        if(n==3)dialog->preview().setIso();++*index;
      });views->start();
    });wait->start();
  });
  QTimer::singleShot(0,&window,[&]{
    if(mode=="saved"||mode=="source-hd")dialog->openSaved(input);
    else if(mode=="texture"){
      dialog->setTextureSession(input);dialog->findChild<QPushButton*>("image3dCheckGpu")->click();
      auto* ready=new QTimer(&window);ready->setInterval(100);
      QObject::connect(ready,&QTimer::timeout,&window,[&,ready]{auto* button=dialog->findChild<QPushButton*>("image3dGenerate");if(button->isEnabled()){ready->stop();button->click();}});ready->start();
      QTimer::singleShot(30000,&window,[&,ready]{if(ready->isActive()){ready->stop();summary["readiness_timeout"]=true;finish(11);}});
    }
    else if(mode=="resume")dialog->backend().resumeCleanup(p3dasset::BackendConfig::read(cli.value("config")),input,out);
    else{
      dialog->findChild<QPushButton*>("image3dCheckGpu")->click();dialog->importImage(input);auto* ready=new QTimer(&window);ready->setInterval(100);
      QObject::connect(ready,&QTimer::timeout,&window,[&,ready]{
        if(dialog->findChild<QPushButton*>("image3dGenerate")->isEnabled()){ready->stop();dialog->generate();}
        else if(dialog->statusText().contains("required") || dialog->statusText().contains("could not")){ready->stop();summary["input_error"]=dialog->statusText();finish(11);}
      });ready->start();
      QTimer::singleShot(30000,&window,[&,ready]{if(ready->isActive()){ready->stop();summary["readiness_timeout"]=true;finish(11);}});
    }
  });
  QTimer::singleShot(1250000,&window,[&]{summary["timeout"]=true;dialog->backend().cancel();finish(12);});
  return app.exec();
}
