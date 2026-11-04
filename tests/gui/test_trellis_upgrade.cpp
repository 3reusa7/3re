#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include "backends/image_input.hpp"
#include "backends/image_material.hpp"
#include "backends/trellis_backend.hpp"
#include "studio/image3d_workspace.hpp"
using namespace p3dasset;
namespace {
pixel3d::Mesh tetra(){return {{{-1,-1,-1},{1,-1,-1},{0,1,-1},{0,0,1}},{{0,2,1},{0,1,3},{1,2,3},{2,0,3}},{},{}};}
QImage art(){QImage im(24,48,QImage::Format_RGBA8888);im.fill(Qt::transparent);for(int y=4;y<44;++y)for(int x=4;x<20;++x)im.setPixelColor(x,y,y<24?Qt::red:Qt::green);return im;}
}
class UpgradeTests:public QObject{
 Q_OBJECT
 private slots:
 void existingImageCases(){
  const auto cases=qEnvironmentVariable("PIXEL3D_TRELLIS_IMAGE_CASES").split(';',Qt::SkipEmptyParts);
  QVERIFY2(cases.size()>=6,"Six saved general-purpose image cases must be supplied");
  for(const auto& file:cases){const auto input=prepare_image(file);QVERIFY2(input.error.isEmpty(),qPrintable(file+": "+input.error));QVERIFY(!input.cutout.isNull());QVERIFY(input.cutout.width()==input.cutout.height());}
 }
 void arbitraryFormatsAndAspect(){
  QTemporaryDir dir;QImage opaque(93,27,QImage::Format_RGB32);opaque.fill(QColor(23,40,70));
  for(const auto& format:{"png","jpg","jpeg","bmp"}){const auto file=dir.filePath(QString("source.")+format);QVERIFY2(opaque.save(file,format),format);
    QFile before(file);QVERIFY(before.open(QIODevice::ReadOnly));const auto hash=QCryptographicHash::hash(before.readAll(),QCryptographicHash::Sha256);before.close();
    const auto input=prepare_image(file);QVERIFY2(input.error.isEmpty(),qPrintable(input.error));QCOMPARE(input.original_size,opaque.size());QVERIFY(input.cutout.width()==input.cutout.height());
    QVERIFY(input.cutout.pixelColor(0,0).alpha()==0);QVERIFY(!input.warning.isEmpty());QVERIFY(before.open(QIODevice::ReadOnly));QCOMPARE(QCryptographicHash::hash(before.readAll(),QCryptographicHash::Sha256),hash);
  }
  QImage large(6000,301,QImage::Format_RGB32);large.fill(Qt::blue);const auto file=dir.filePath("large.png");QVERIFY(large.save(file));const auto input=prepare_image(file);
  QVERIFY2(input.error.isEmpty(),qPrintable(input.error));QCOMPARE(input.original_size,large.size());QVERIFY(input.cutout.width()<=1130);QCOMPARE(bounded_image_size({100000,1},2048),QSize(2048,1));
  QImage strip(6000,1,QImage::Format_RGB32);strip.fill(Qt::red);const auto narrow=dir.filePath("strip.png");QVERIFY(strip.save(narrow));const auto extreme=prepare_image(narrow);
  QVERIFY(extreme.error.isEmpty());bool visible=false;for(int y=0;y<extreme.cutout.height();++y)for(int x=0;x<extreme.cutout.width();++x)visible|=extreme.cutout.pixelColor(x,y).alpha()>0;QVERIFY(visible);
  const auto controlled=qEnvironmentVariable("PIXEL3D_TRELLIS_CONTROLLED_INPUT");
  if(!controlled.isEmpty()){
    const auto source=qEnvironmentVariable("PIXEL3D_TRELLIS_IMAGE_CASES").split(';').first();
    QCOMPARE(prepare_image(source).cutout,QImage(controlled).convertToFormat(QImage::Format_RGBA8888));
  }
 }
 void alphaFramingAndPixelPalette(){
  QTemporaryDir dir;const auto file=dir.filePath("alpha.png");QVERIFY(art().save(file));const auto input=prepare_image(file,{InputType::PixelArt,true});
  QVERIFY(input.error.isEmpty());QVERIFY(input.crisp);QCOMPARE(input.subject_bounds.size(),QSize(16,40));
  int minx=999,miny=999,maxx=-1,maxy=-1;
  for(int y=0;y<input.cutout.height();++y)for(int x=0;x<input.cutout.width();++x){const auto c=input.cutout.pixelColor(x,y);if(c.alpha()){QVERIFY(c==QColor(Qt::red)||c==QColor(Qt::green));minx=std::min(minx,x);miny=std::min(miny,y);maxx=std::max(maxx,x);maxy=std::max(maxy,y);}}
  QCOMPARE(QSize(maxx-minx+1,maxy-miny+1),QSize(16,40));QVERIFY(minx>0 && miny>0);
 }
 void sourceTextureRoundtripPreservesGeometry(){
  QTemporaryDir dir;const auto original=tetra();const auto colored=project_source_material(original,art(),512,true);
  QCOMPARE(colored->positions,original.positions);QCOMPARE(colored->indices.size(),original.tris.size()*3);QVERIFY(colored->texture);
  QCOMPARE(colored->uvs[2].y,0.);QCOMPARE(colored->uvs[0].y,1.);
  const auto file=dir.filePath("Textured.glb");save_source_material(*colored,file);
  const auto loaded=load_geometry_glb(std::filesystem::path(file.toStdWString()));QCOMPARE(loaded.positions,original.positions);QCOMPARE(loaded.tris,original.tris);
  const auto material=load_generated_appearance(std::filesystem::path(file.toStdWString()));QVERIFY(material);QVERIFY(*material->texture==*colored->texture);
  MeshAsset asset;asset.geometry=std::make_shared<const pixel3d::Mesh>(original);asset.geometry_glb=std::filesystem::path(file.toStdWString());asset.appearance=colored;asset.geometry_only=false;asset.validation=Validation::Review;
  asset.report=std::filesystem::path(dir.filePath("report.json").toStdWString());QFile report(dir.filePath("report.json"));QVERIFY(report.open(QIODevice::WriteOnly));report.write("{\"status\":\"REVIEW\",\"material_note\":\"approximate\"}");report.close();
  QString error;QVERIFY(!TrellisBackend::exportAsset(asset,dir.filePath("Exported.glb"),false,error));QVERIFY2(TrellisBackend::exportAsset(asset,dir.filePath("Exported.glb"),true,error),qPrintable(error));
  QVERIFY(load_generated_appearance(std::filesystem::path(dir.filePath("Exported.glb").toStdWString())));
  QVERIFY_EXCEPTION_THROWN(save_source_material(*colored,file),std::runtime_error);
 }
 void honestProfilesAndRelativeSetup(){
  QTemporaryDir dir;QFile file(dir.filePath("setup.json"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("{\"runtime_dir\":\"TrellisRuntime\",\"models_dir\":\"weights\",\"post_executable\":\"TrellisRuntime/post.exe\",\"weights_manifest\":\"manifest.json\",\"engine_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"post_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}");file.close();
  const auto config=BackendConfig::read(file.fileName());QVERIFY(QDir::isAbsolutePath(config.runtime_dir));QCOMPARE(config.runtime_dir,dir.filePath("TrellisRuntime"));
  p3dgui::Image3DWorkspace dialog(true);const auto profiles=dialog.findChild<QComboBox*>("image3dProfile"),res=dialog.findChild<QComboBox*>("image3dResolution");QVERIFY(profiles);QVERIFY(res);
  QCOMPARE(profiles->count(),4);QVERIFY(profiles->model()->flags(profiles->model()->index(0,0))&Qt::ItemIsEnabled);QVERIFY(profiles->model()->flags(profiles->model()->index(1,0))&Qt::ItemIsEnabled);
  QVERIFY(!(profiles->model()->flags(profiles->model()->index(2,0))&Qt::ItemIsEnabled));QVERIFY(!(res->model()->flags(res->model()->index(1,0))&Qt::ItemIsEnabled));QVERIFY(!res->itemData(1,Qt::ToolTipRole).toString().isEmpty());
 }
 void cleanupFailureKeepsRawWithoutInference(){
  if(available_ram()<5ull*1024*1024*1024/2)QSKIP("Unchanged 2.5 GiB saved-asset validation gate");
  QTemporaryDir dir;const auto colored=project_source_material(tetra(),art(),512,true);const auto raw=dir.filePath("raw.glb");save_source_material(*colored,raw);
  QFile ply(dir.filePath("raw.ply"));QVERIFY(ply.open(QIODevice::WriteOnly));ply.write("deliberate invalid PLY fixture");ply.close();
  BackendConfig config;config.post_executable=QDir(QCoreApplication::applicationDirPath()).filePath("pixel3d-asset-inspect.exe");QFile executable(config.post_executable);QVERIFY(executable.open(QIODevice::ReadOnly));config.post_sha256=QString::fromLatin1(QCryptographicHash::hash(executable.readAll(),QCryptographicHash::Sha256).toHex());
  TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);QVERIFY(backend.resumeCleanup(config,raw,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,45000);
  QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));QVERIFY(backend.report().value("cleanup_failed").toBool());QVERIFY(!backend.report().value("generation_executed").toBool());QVERIFY(backend.asset().geometry);QVERIFY(QFileInfo(raw).isFile());
 }
};
int main(int argc,char** argv){qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);QTemporaryDir settings;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());UpgradeTests tests;return QTest::qExec(&tests,argc,argv);}
#include "test_trellis_upgrade.moc"
