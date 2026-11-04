#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <fstream>
#include <numbers>
#include <cstring>
#include <functional>
#include <set>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#include "backends/trellis_backend.hpp"
#include "backends/trellis_native_post.hpp"
#include "backends/image_material.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/studio_window.hpp"
#include "main_window.hpp"
#include "viewport.hpp"

using namespace p3dasset;
using nlohmann::json;
namespace {
std::filesystem::path native(const QString& p){return std::filesystem::path(p.toStdWString());}
void writeGlb(const QString& path,const std::vector<std::array<float,3>>& positions,const std::vector<std::array<uint32_t,3>>& faces){
  std::vector<float> lo(positions.front().begin(),positions.front().end()),hi=lo;
  for(auto p:positions)for(int k=0;k<3;++k){lo[size_t(k)]=std::min(lo[size_t(k)],p[size_t(k)]);hi[size_t(k)]=std::max(hi[size_t(k)],p[size_t(k)]);}
  const size_t vertex_bytes=positions.size()*12,index_bytes=faces.size()*12;
  json j={{"asset",{{"version","2.0"}}},{"scene",0},{"scenes",json::array({{{"nodes",{0}}}})},
    {"nodes",json::array({{{"mesh",0}}})},{"meshes",json::array({{{"primitives",json::array({{{"attributes",{{"POSITION",0}}},{"indices",1},{"mode",4}}})}}})},
    {"buffers",json::array({{{"byteLength",vertex_bytes+index_bytes}}})},
    {"bufferViews",json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",vertex_bytes},{"target",34962}},
                               {{"buffer",0},{"byteOffset",vertex_bytes},{"byteLength",index_bytes},{"target",34963}}})},
    {"accessors",json::array({{{"bufferView",0},{"componentType",5126},{"count",positions.size()},{"type","VEC3"},{"min",lo},{"max",hi}},
                              {{"bufferView",1},{"componentType",5125},{"count",faces.size()*3},{"type","SCALAR"}}})}};
  auto text=j.dump();while(text.size()%4)text+=' ';
  const uint32_t header[]={0x46546c67,2,uint32_t(12+8+text.size()+8+vertex_bytes+index_bytes)};
  const uint32_t json_chunk[]={uint32_t(text.size()),0x4e4f534a};
  const uint32_t bin_chunk[]={uint32_t(vertex_bytes+index_bytes),0x004e4942};
  std::ofstream out(native(path),std::ios::binary);out.write(reinterpret_cast<const char*>(header),12);
  out.write(reinterpret_cast<const char*>(json_chunk),8);out.write(text.data(),std::streamsize(text.size()));
  out.write(reinterpret_cast<const char*>(bin_chunk),8);out.write(reinterpret_cast<const char*>(positions.data()),std::streamsize(vertex_bytes));
  out.write(reinterpret_cast<const char*>(faces.data()),std::streamsize(index_bytes));
  if(!out)throw std::runtime_error("Could not write fixture");
}
QString tetra(const QTemporaryDir& dir){
  const auto file=dir.filePath("tetra.glb");writeGlb(file,{{0,0,0},{1,0,0},{0,1,0},{0,0,1}},{{0,2,1},{0,1,3},{1,2,3},{2,0,3}});return file;
}
// Closed UV sphere (enough surface for Auto Texture) beside a picture of it: left half red, right half blue.
QString sphereWithPicture(const QTemporaryDir& dir){
  std::vector<std::array<float,3>> p;std::vector<std::array<uint32_t,3>> f;const int rings=24,segments=48;
  p.push_back({0,0.5f,0});
  for(int r=1;r<rings;++r)for(int s=0;s<segments;++s){const double a=std::numbers::pi*r/rings,b=2*std::numbers::pi*s/segments;p.push_back({float(0.5*std::sin(a)*std::sin(b)),float(0.5*std::cos(a)),float(0.5*std::sin(a)*std::cos(b))});}
  p.push_back({0,-0.5f,0});const uint32_t last=uint32_t(p.size()-1);
  auto at=[&](int r,int s){return uint32_t(1+(r-1)*segments+(s%segments));};
  for(int s=0;s<segments;++s)f.push_back({0,at(1,s),at(1,s+1)});
  for(int r=1;r<rings-1;++r)for(int s=0;s<segments;++s){f.push_back({at(r,s),at(r+1,s),at(r+1,s+1)});f.push_back({at(r,s),at(r+1,s+1),at(r,s+1)});}
  for(int s=0;s<segments;++s)f.push_back({at(rings-1,s),last,at(rings-1,s+1)});
  const auto file=dir.filePath("geometry.glb");writeGlb(file,p,f);
  QImage picture(256,256,QImage::Format_RGBA8888);picture.fill(Qt::transparent);
  for(int y=0;y<256;++y)for(int x=0;x<256;++x)if((x-127.5)*(x-127.5)+(y-127.5)*(y-127.5)<110*110)picture.setPixelColor(x,y,x<128?QColor(220,30,30):QColor(30,60,220));
  picture.save(dir.filePath("input.png"));return file;
}
// TRELLIS-like texture volume of the sphere next to its GLB (engine POST layout, no mesh rows): the picture's
// colours on the front half, green on the back half, darker as the engine's albedo comes out.
void nativePostForSphere(const QTemporaryDir& dir){
  constexpr int res=128;std::set<std::array<int32_t,3>> voxels;std::vector<float> rows;std::vector<int32_t> coords;
  for(int i=0;i<=400;++i)for(int j=0;j<800;++j){
    const double a=std::numbers::pi*i/400,b=2*std::numbers::pi*j/800;
    const double x=0.5*std::sin(a)*std::sin(b),y=0.5*std::cos(a),z=0.5*std::sin(a)*std::cos(b);
    const double t[3]={x,-z,y};std::array<int32_t,3> c;
    for(int k=0;k<3;++k)c[size_t(k)]=std::clamp(int32_t(std::floor((t[k]+0.5)*res)),0,res-1);
    if(!voxels.insert(c).second)continue;
    coords.insert(coords.end(),c.begin(),c.end());
    // Front half: the picture's colours (sRGB); back half: its own colour (green), not a copy of the front.
    const float r=z<0?0.2f:x<0?0.86f:0.12f,g=z<0?0.7f:x<0?0.12f:0.24f,bl=z<0?0.25f:x<0?0.12f:0.86f;
    for(float v:{r*0.7f,g*0.7f,bl*0.7f,0.f,1.f,1.f})rows.push_back(v);  // ... darkened like the engine's albedo
  }
  std::ofstream out(native(dir.filePath("native.post")),std::ios::binary);
  const int32_t h[4]{0,0,int32_t(voxels.size()),res};out.write(reinterpret_cast<const char*>(h),16);
  out.write(reinterpret_cast<const char*>(coords.data()),std::streamsize(coords.size()*4));
  out.write(reinterpret_cast<const char*>(rows.data()),std::streamsize(rows.size()*4));
}
QString crossing(const QTemporaryDir& dir){
  const auto file=dir.filePath("crossing.glb");
  writeGlb(file,{{-1,-1,0},{1,-1,0},{0,1,0},{0,-.5f,-1},{0,-.5f,1},{0,.7f,0}},{{0,1,2},{3,4,5}});return file;
}
json safeTopology(){return {{"complete",true},{"vertices",4},{"triangles",4},{"boundary_edges",0},
  {"overfull_edges",0},{"invalid_vertex_links",0},{"inconsistent_winding_edges",0},{"duplicate_faces",0},{"internal_faces",0},{"zero_area_faces",0}};}
json safeIntersections(){return {{"complete",true},{"vertices",4},{"faces",4},{"pair_classification",json::object()},{"edge_star_classification",json::object()}};}
bool resources(){return available_ram()>=3ull*1024*1024*1024/2;}
void changeJson(const QString& source,const QString& destination,const std::function<void(json&)>& mutate){
  QFile file(source);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Fixture read failed");
  const auto bytes=file.readAll();uint32_t length=0;std::memcpy(&length,bytes.constData()+12,4);
  auto j=json::parse(bytes.constData()+20,bytes.constData()+20+length);mutate(j);
  auto text=j.dump();while(text.size()%4)text+=' ';
  const auto bin=bytes.mid(20+int(length));const uint32_t header[]={0x46546c67,2,uint32_t(20+text.size()+size_t(bin.size())),uint32_t(text.size()),0x4e4f534a};
  QFile out(destination);if(!out.open(QIODevice::WriteOnly))throw std::runtime_error("Fixture write failed");
  out.write(reinterpret_cast<const char*>(header),20);out.write(text.data(),qint64(text.size()));out.write(bin);
}
}
class Image3DTests:public QObject{
  Q_OBJECT
 private slots:
  void highResolutionAppearanceDoesNotChangeInference(){
    QTemporaryDir dir;QImage original(1536,1536,QImage::Format_RGBA8888);for(int y=0;y<1536;++y)for(int x=0;x<1536;++x)original.setPixelColor(x,y,QColor((x%2)*255,(y%2)*255,73,255));
    auto file=dir.filePath("detail.png");QVERIFY(original.save(file));auto input=prepare_image(file,ImageInputOptions{InputType::PixelArt,false});QVERIFY(input.error.isEmpty());QCOMPARE(input.appearance,original);QCOMPARE(input.cutout.size(),QSize(1126,1126));
    pixel3d::Mesh mesh;mesh.positions={{0,0,0},{1,0,0},{0,1,0}};mesh.tris={{0,1,2}};auto hd=project_source_material(mesh,input.appearance,2048,true);QCOMPARE(hd->texture->width(),1536);QCOMPARE(hd->texture->height(),1536);QCOMPARE(hd->texture->at(101,100).r,uint8_t(255));QCOMPARE(hd->texture->at(101,100).b,uint8_t(73));
    auto legacy=project_source_material(mesh,input.cutout,1024,true);QCOMPARE(legacy->texture->height(),1024);QCOMPARE(hd->positions,legacy->positions);QCOMPARE(hd->indices,legacy->indices);
    save_source_material(*hd,dir.filePath("hd.glb"));auto back=load_generated_appearance(native(dir.filePath("hd.glb")));QVERIFY(back);QVERIFY(*hd->texture==*back->texture);
  }

  void launcherConfigAndLowMemoryPreview(){
    QTemporaryDir dir;const auto config=dir.filePath("backend.json");QFile setup(config);
    QVERIFY(setup.open(QIODevice::WriteOnly));setup.write("{}");setup.close();
    const auto previous=qgetenv("PIXEL3D_TRELLIS_CONFIG");
    struct RestoreEnv{QByteArray value;~RestoreEnv(){if(value.isNull())qunsetenv("PIXEL3D_TRELLIS_CONFIG");else qputenv("PIXEL3D_TRELLIS_CONFIG",value);}} restore{previous};
    QSettings().setValue("trellis/setup",dir.filePath("stale.json"));qputenv("PIXEL3D_TRELLIS_CONFIG",config.toUtf8());
    p3dgui::Image3DWorkspace dialog(true);dialog.setProjectsDir(dir.filePath("Projects"));dialog.show();
    QCOMPARE(dialog.findChild<QLineEdit*>("image3dSetup")->text(),config);
    auto* memory=dialog.findChild<QLabel*>("image3dMemory");QVERIFY(memory);QVERIFY(!memory->text().isEmpty());
    QImage input(16,16,QImage::Format_RGBA8888);input.fill(Qt::transparent);
    for(int y=3;y<13;++y)for(int x=3;x<13;++x)input.setPixelColor(x,y,Qt::yellow);
    const auto image=dir.filePath("input.png");QVERIFY(input.save(image));dialog.importImage(image);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.statusText().startsWith("Cutout ready"),5000);
    auto* generate=dialog.findChild<QPushButton*>("image3dGenerate");QVERIFY(generate);
    if(available_ram()<generation_admission_ram()){
      QVERIFY(!generate->isEnabled());QVERIFY(memory->text().contains("Generation disabled"));
      dialog.generate();QVERIFY(dialog.statusText().contains("Generation disabled"));QVERIFY(!dialog.backend().busy());
      QVERIFY(!dialog.backend().report().value("generation_executed").toBool());
    }else {QVERIFY(!generate->isEnabled());QVERIFY(dialog.findChild<QLabel*>("image3dGpuStatus")->text().contains("not checked"));}
    const auto evidence=qEnvironmentVariable("PIXEL3D_TEST_EVIDENCE");
    if(!evidence.isEmpty()){QVERIFY(QDir().mkpath(evidence));QVERIFY(dialog.grab().save(QDir(evidence).filePath("image-preview-memory-gate.png")));}
    QSettings().remove("trellis/setup");dialog.cancel();
  }
  void immutableGenerationProfile(){
    QCOMPARE(generation_admission_ram(),13ull*1024*1024*1024/4);
    BackendConfig c;c.models_dir="existing weights";
    const auto args=TrellisBackend::inferenceArguments(c,"cutout.png","raw.glb",42,"stages");
    for(auto value:{"--low-vram-4gb","--require-gpu","--no-texture","--no-fa","--capture-stages"})QVERIFY(args.contains(value));
    QCOMPARE(args[args.indexOf("--res")+1],QString("512"));QCOMPARE(args[args.indexOf("--steps")+1],QString("12"));
    QCOMPARE(args[args.indexOf("--model")+1],QString("trellis"));QVERIFY(!args.contains("pixal3d"));
    // Hybrid texture: the same profile with the texture stage on; stage capture is incompatible and dropped.
    const auto tex=TrellisBackend::inferenceArguments(c,"cutout.png","raw.glb",42,"stages",true);
    QVERIFY(!tex.contains("--no-texture"));QVERIFY(!tex.contains("--capture-stages"));
    for(auto value:{"--low-vram-4gb","--require-gpu","--no-fa"})QVERIFY(tex.contains(value));
    QCOMPARE(tex[tex.indexOf("--res")+1],QString("512"));QCOMPARE(tex[tex.indexOf("--steps")+1],QString("12"));QCOMPARE(tex[tex.indexOf("--seed")+1],QString("42"));
    QCOMPARE(tex.size(),args.size()-3);
  }
  void nativeTextureDumpSplitsIntoTheGeometryRunFiles(){
    // A POST dump (engine layout) -> raw.ply in trellis write_ply format and raw.glb with (x, z, -y) positions.
    QTemporaryDir dir;
    const std::vector<float> verts{0,0,0, 1,0,0, 0,1,0, 0,0,1};
    const std::vector<int32_t> faces{0,2,1, 0,1,3, 0,3,2, 1,2,3};
    const std::vector<int32_t> coords{1,2,3, 4,5,6};
    const std::vector<float> pbr{.1f,.2f,.3f,0,1,1, .4f,.5f,.6f,0,1,1};
    const auto post=dir.filePath("native.post");
    {
      std::ofstream out(native(post),std::ios::binary);
      const int32_t h[4]{4,4,2,512};out.write(reinterpret_cast<const char*>(h),16);
      out.write(reinterpret_cast<const char*>(verts.data()),48);out.write(reinterpret_cast<const char*>(faces.data()),48);
      out.write(reinterpret_cast<const char*>(coords.data()),24);out.write(reinterpret_cast<const char*>(pbr.data()),48);
    }
    const auto split=split_native_post(native(post),native(dir.filePath("raw.ply")),native(dir.filePath("raw.glb")),42);
    QCOMPARE(split.vertices,int64_t(4));QCOMPARE(split.triangles,int64_t(4));QCOMPARE(split.voxels,int64_t(2));QCOMPARE(split.resolution,512);
    QFile ply(dir.filePath("raw.ply"));QVERIFY(ply.open(QIODevice::ReadOnly));const auto bytes=ply.readAll();
    const QByteArray header("ply\nformat binary_little_endian 1.0\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                            "element face 4\nproperty list uchar int vertex_indices\nend_header\n");
    QVERIFY(bytes.startsWith(header));QCOMPARE(bytes.size(),qsizetype(header.size()+4*12+4*13));
    QCOMPARE(std::memcmp(bytes.constData()+header.size(),verts.data(),48),0);
    QCOMPARE(int(uint8_t(bytes[header.size()+48])),3);QCOMPARE(std::memcmp(bytes.constData()+header.size()+49,faces.data(),12),0);
    const auto mesh=load_geometry_glb(native(dir.filePath("raw.glb")));
    QCOMPARE(mesh.positions.size(),size_t(4));QCOMPARE(mesh.tris.size(),size_t(4));
    QCOMPARE(mesh.positions[3].y,1.0);QCOMPARE(mesh.positions[2].z,-1.0);  // (x, z, -y)
    QVERIFY_EXCEPTION_THROWN(split_native_post(native(post),native(dir.filePath("raw.ply")),native(dir.filePath("other.glb")),42),std::exception);
    {std::ofstream cut(native(dir.filePath("cut.post")),std::ios::binary);const int32_t h[4]{4,4,2,512};cut.write(reinterpret_cast<const char*>(h),16);}
    QVERIFY_EXCEPTION_THROWN(split_native_post(native(dir.filePath("cut.post")),native(dir.filePath("c.ply")),native(dir.filePath("c.glb")),42),std::exception);
  }
  void savedPreviewBudgetAndMalformedResources(){
    QTemporaryDir dir;const auto source=tetra(dir);
    const auto small=saved_preview_memory(native(source));QVERIFY(small.bounded_profile);
    QCOMPARE(small.vertices,uint64_t(4));QCOMPARE(small.triangles,uint64_t(4));
    QCOMPARE(small.required_free_bytes,3ull*1024*1024*1024/2);
    QVERIFY(small.required_free_bytes<5ull*1024*1024*1024/2);
    const auto external=dir.filePath("external.glb");changeJson(source,external,[](json& j){j["buffers"][0]["uri"]="unread-external.bin";});
    QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(external)),std::exception);
    const auto overflow=dir.filePath("overflow.glb");changeJson(source,overflow,[](json& j){j["accessors"][0]["count"]=uint64_t(-1);});
    QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(overflow)),std::exception);
    const auto truncated=dir.filePath("truncated.glb");QFile bad(truncated);QVERIFY(bad.open(QIODevice::WriteOnly));bad.write("bad");bad.close();
    QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(truncated)),std::exception);
    const auto large=dir.filePath("large.glb");QVERIFY(QFile::copy(source,large));std::filesystem::resize_file(native(large),16ull*1024*1024+1);
    const auto conservative=saved_preview_memory(native(large));QVERIFY(!conservative.bounded_profile);
    QCOMPARE(conservative.required_free_bytes,5ull*1024*1024*1024/2);
    QImage image(16,16,QImage::Format_RGBA8888);image.fill(Qt::yellow);
    const auto mesh=load_geometry_glb(native(source));const auto colored=project_source_material(mesh,image,16,false);
    const auto textured=dir.filePath("textured.glb");save_source_material(*colored,textured);
    const auto textureBudget=saved_preview_memory(native(textured));QVERIFY(textureBudget.bounded_profile);
    QVERIFY(textureBudget.decoded_image_bytes>0);QVERIFY(textureBudget.decoded_image_bytes<=16*16*4);
    const auto imageOverrun=dir.filePath("image-overrun.glb");changeJson(textured,imageOverrun,[](json& j){
      const auto i=j["images"][0]["bufferView"].get<size_t>();j["bufferViews"][i]["byteLength"]=uint64_t(-1);
    });QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(imageOverrun)),std::exception);
    const auto imageOffset=dir.filePath("image-offset.glb");changeJson(textured,imageOffset,[](json& j){
      const auto i=j["images"][0]["bufferView"].get<size_t>();j["bufferViews"][i]["byteOffset"]=j["buffers"][0]["byteLength"].get<uint64_t>()-1;
    });QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(imageOffset)),std::exception);
    // Edit PNG IHDR width only. Header inspection must reject before decoding
    // pixels; no large image allocation or geometry change is necessary.
    QFile png(textured);QVERIFY(png.open(QIODevice::ReadOnly));auto bytes=png.readAll();png.close();uint32_t length=0;std::memcpy(&length,bytes.constData()+12,4);
    const auto j=json::parse(bytes.constData()+20,bytes.constData()+20+length);
    const auto view=j["images"][0]["bufferView"].get<size_t>();
    const auto offset=j["bufferViews"][view].value("byteOffset",size_t(0));
    const auto width=20+size_t(length)+8+offset+16;QVERIFY(width+4<=size_t(bytes.size()));
    // 8192 px wide: over the 4096 limit of character textures (3re 2.3).
    bytes[qsizetype(width)]=0;bytes[qsizetype(width+1)]=0;bytes[qsizetype(width+2)]=32;bytes[qsizetype(width+3)]=0;
    const auto oversized=dir.filePath("oversized-image.glb");QFile over(oversized);QVERIFY(over.open(QIODevice::WriteOnly));QCOMPARE(over.write(bytes),bytes.size());over.close();
    QVERIFY_EXCEPTION_THROWN(saved_preview_memory(native(oversized)),std::exception);
  }
  void transparencyIsRealAndHighlightsSurvive(){
    QTemporaryDir dir;QImage input(9,9,QImage::Format_RGBA8888);input.fill(Qt::white);
    for(int y=2;y<7;++y)for(int x=2;x<7;++x)input.setPixelColor(x,y,Qt::black);
    input.setPixelColor(4,4,Qt::white);const auto file=dir.filePath("object.png");QVERIFY(input.save(file));
    QVERIFY(prepare_image(file,false).error.isEmpty());const auto cut=prepare_image(file,true);
    QVERIFY(cut.error.isEmpty());QCOMPARE(cut.cutout.pixelColor(0,0).alpha(),0);
    bool highlight=false,black=false;for(int y=0;y<cut.cutout.height();++y)for(int x=0;x<cut.cutout.width();++x){const auto c=cut.cutout.pixelColor(x,y);highlight|=c==QColor(Qt::white);black|=c==QColor(Qt::black);}
    QVERIFY(highlight);QVERIFY(black);QVERIFY(!cut.foreground_touches_border);
    const auto png=dir.filePath("alpha.png");QVERIFY(cut.cutout.save(png));QVERIFY(prepare_image(png,false).error.isEmpty());
  }
  void unsafeAndUnknownNeverPass(){
    auto t=safeTopology(),x=safeIntersections();QCOMPARE(classify_validation(t,x),Validation::Pass);
    x["pair_classification"]["transverse_crossing"]=1;QCOMPARE(classify_validation(t,x),Validation::Unsafe);
    x=safeIntersections();t["overfull_edges"]=1;QCOMPARE(classify_validation(t,x),Validation::Unsafe);
    x["complete"]=false;QCOMPARE(classify_validation(t,x),Validation::Unsafe);
    x=safeIntersections();
    t=safeTopology();t["boundary_edges"]=3;QCOMPARE(classify_validation(t,x),Validation::Review);
    t=safeTopology();x["complete"]=false;QCOMPARE(classify_validation(t,x),Validation::Unknown);
    x=safeIntersections();x["pair_classification"]["new_unverified_kind"]=5;QCOMPARE(classify_validation(t,x),Validation::Unknown);
    x=safeIntersections();x["edge_star_classification"]["tolerance_sensitive"]=1;QCOMPARE(classify_validation(t,x),Validation::Review);
    x=safeIntersections();x["faces"]=99;QCOMPARE(classify_validation(t,x),Validation::Unknown);
  }
  void geometryRoundtripRejectsInvalidBuffers(){
    QTemporaryDir dir;auto mesh=load_geometry_glb(native(tetra(dir)));QCOMPARE(mesh.tris.size(),size_t(4));
    auto file=native(dir.filePath("mesh.bin"));write_geometry_binary(mesh,file);const auto restored=read_geometry_binary(file);
    QVERIFY(restored.positions==mesh.positions);QVERIFY(restored.tris==mesh.tris);
    mesh.tris[0][0]=999;QVERIFY_EXCEPTION_THROWN(write_geometry_binary(mesh,native(dir.filePath("bad.bin"))),std::runtime_error);
    QFile broken(QString::fromStdWString(file.wstring()));QVERIFY(broken.open(QIODevice::WriteOnly));broken.write("bad");broken.close();
    QVERIFY_EXCEPTION_THROWN(read_geometry_binary(file),std::runtime_error);
  }
  void independentWorkflowLeavesDrawingProjectAlone(){
    p3dgui::WindowOptions options;options.software_view=true;p3dgui::StudioWindow studio(options);auto& window=studio.pixelBuilder();
    const auto before=window.controller().view().doc.revision;QVERIFY(window.hasAction("image3d"));
#ifndef PIXEL3D_ENABLE_SMARTQUAD
    QVERIFY(!window.hasAction("smart_quad"));
#endif
    window.runAction("image3d");QCOMPARE(studio.currentPage(),QString("image"));QCOMPARE(studio.imageMode(),QString("ai"));auto* dialog=&studio.imageWorkspace();
    QVERIFY(!dialog->findChild<QPushButton*>("image3dGenerate")->isEnabled());
    QCOMPARE(window.controller().view().doc.revision,before);dialog->cancel();window.controller().cleanShutdown();
  }
  void experimentalTextureSessionMissingDoesNotLaunch(){
    if(available_ram()<generation_admission_ram())QSKIP("Inference admission remains unchanged");
    QTemporaryDir dir;TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    QVERIFY(backend.generateTexture({},dir.filePath("missing.json"),dir.path()));
    QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,5000);QVERIFY(!done[0][0].toBool());
    QVERIFY(!backend.report().value("worker_launched").toBool());QVERIFY(!backend.report().value("inference_started").toBool());
    QVERIFY(backend.report().value("texture_only").toBool());QVERIFY(!backend.report().value("geometry_inference").toBool());
  }
  void savedAssetExportReimportAndHeartbeat(){
    if(!resources())QSKIP("1.5 GiB bounded saved-preview preflight required");
    QTemporaryDir dir;const auto source=tetra(dir);TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    int beats=0;QTimer heartbeat;heartbeat.setInterval(1);connect(&heartbeat,&QTimer::timeout,[&]{++beats;});heartbeat.start();
    QVERIFY(backend.openSaved(source,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,45000);
    QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));QCOMPARE(backend.asset().validation,Validation::Pass);QVERIFY(beats>0);
    QVERIFY(!backend.report().value("generation_executed").toBool());QVERIFY(backend.asset().geometry_only);
    QString error;const auto output=dir.filePath("export.glb");QVERIFY2(backend.exportGlb(output,false,error),qPrintable(error));
    const auto result=load_geometry_glb(native(output));QVERIFY(result.tris==backend.asset().geometry->tris);QVERIFY(result.positions==backend.asset().geometry->positions);
    QFile a(source),b(output);QVERIFY(a.open(QIODevice::ReadOnly));QVERIFY(b.open(QIODevice::ReadOnly));QCOMPARE(a.readAll(),b.readAll());
    QVERIFY(!backend.exportGlb(output,false,error));
  }
  void savedGeometryIsAutoTexturedAndExported(){
    if(!resources())QSKIP("1.5 GiB bounded saved-preview preflight required");
    QTemporaryDir dir;const auto source=sphereWithPicture(dir);TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    QVERIFY(backend.openSaved(source,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,120000);
    QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));
    // Without TRELLIS's texture the hidden half is extrapolated from the picture's edges: delivered, but FAIL.
    const auto auto_texture=backend.report().value("auto_texture").toObject();
    QCOMPARE(auto_texture.value("selected").toString(),QString("auto_texture"));
    QCOMPARE(auto_texture.value("verdict").toString(),QString("FAIL"));
    QVERIFY(QJsonDocument(auto_texture.value("issues").toArray()).toJson().contains("extrapolated from the picture's visible edges"));
    QVERIFY(!backend.report().value("auto_texture_failed").toBool());
    QCOMPARE(backend.asset().materials.value("kind",std::string()),std::string("auto_texture"));
    QVERIFY(!backend.asset().geometry_only && backend.asset().appearance);
    // Geometry untouched; the exported file carries the texture and re-imports identically.
    QVERIFY(geometrically_identical(*backend.asset().geometry,load_geometry_glb(native(source))));
    QString error;const auto output=dir.filePath("export.glb");QVERIFY2(backend.exportGlb(output,true,error),qPrintable(error));
    QVERIFY(geometrically_identical(load_geometry_glb(native(output)),load_geometry_glb(native(source))));
    QCOMPARE(material_info(native(output)).value("kind",std::string()),std::string("auto_texture"));
    QVERIFY(QFileInfo::exists(QDir(backend.runDirectory()).filePath("textured.glb.texture.json")));
  }
  void savedGeometryWithTrellisTextureIsHybrid(){
    if(!resources())QSKIP("1.5 GiB bounded saved-preview preflight required");
    QTemporaryDir dir;const auto source=sphereWithPicture(dir);nativePostForSphere(dir);
    TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    QVERIFY(backend.openSaved(source,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,120000);
    QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));QVERIFY(done[0][1].toString().contains("hybrid texture"));
    const auto auto_texture=backend.report().value("auto_texture").toObject();
    QCOMPARE(auto_texture.value("selected").toString(),QString("auto_texture"));
    QVERIFY2(auto_texture.value("verdict").toString()!="FAIL",qPrintable(QJsonDocument(auto_texture.value("issues").toArray()).toJson()));
    QVERIFY(auto_texture.value("native_fraction").toDouble()>0.3);
    QVERIFY(auto_texture.value("native").toObject().value("used").toBool());
    QVERIFY(backend.report().value("material_note").toString().startsWith("Hybrid Texture"));
    QVERIFY(geometrically_identical(*backend.asset().geometry,load_geometry_glb(native(source))));
    QString error;const auto output=dir.filePath("export.glb");QVERIFY2(backend.exportGlb(output,true,error),qPrintable(error));
    QVERIFY(geometrically_identical(load_geometry_glb(native(output)),load_geometry_glb(native(source))));
  }
  void confirmedCrossingRequiresAcknowledgement(){
    if(!resources())QSKIP("1.5 GiB bounded saved-preview preflight required");
    QTemporaryDir dir;TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    QVERIFY(backend.openSaved(crossing(dir),dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,45000);
    QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));QCOMPARE(backend.asset().validation,Validation::Unsafe);
    QString error;const auto file=dir.filePath("issues.glb");QVERIFY(!backend.exportGlb(file,false,error));QVERIFY(!QFileInfo::exists(file));
    QVERIFY(backend.exportGlb(file,true,error));QCOMPARE(load_geometry_glb(native(file)).tris.size(),size_t(2));
  }
  void cancellationBeforeWorkerStartsCanRecover(){
    if(!resources())QSKIP("1.5 GiB bounded saved-preview preflight required");
    QTemporaryDir dir;const auto source=tetra(dir);TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);
    QVERIFY(backend.openSaved(source,dir.path()));backend.cancel();QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,5000);
    QVERIFY(!done[0][0].toBool());QTest::qWait(100);done.clear();
    QVERIFY(backend.openSaved(source,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(done.count()==1,45000);QVERIFY(done[0][0].toBool());
  }
  void cancellationAfterAuditWorkerStartsCanRecover(){
#ifdef Q_OS_WIN
    if(!resources())QSKIP("Bounded saved-preview resources required");
    QTemporaryDir dir;std::vector<std::array<float,3>> vertices;std::vector<std::array<uint32_t,3>> faces;
    for(int y=0;y<128;++y)for(int x=0;x<128;++x)vertices.push_back({float(x),float(y),0});
    for(uint32_t y=0;y<127;++y)for(uint32_t x=0;x<127;++x){const auto i=y*128+x;faces.push_back({i,i+1,i+129});faces.push_back({i,i+129,i+128});}
    const auto source=dir.filePath("grid.glb");writeGlb(source,vertices,faces);
    TrellisBackend backend;QSignalSpy done(&backend,&TrellisBackend::finished);QTimer poll;poll.setInterval(5);DWORD observed=0;
    connect(&backend,&TrellisBackend::logLine,this,[&](const QString& line){if(!observed && line=="Starting pixel3d-geometry-audit.exe")poll.start();});
    connect(&poll,&QTimer::timeout,this,[&]{
      const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return;
      PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
      if(Process32FirstW(snapshot,&entry))do{
        if(entry.th32ParentProcessID==GetCurrentProcessId() && QString::fromWCharArray(entry.szExeFile)=="pixel3d-geometry-audit.exe"){observed=entry.th32ProcessID;break;}
      }while(Process32NextW(snapshot,&entry));CloseHandle(snapshot);
      if(observed){poll.stop();backend.cancel();}
    });
    QVERIFY(backend.openSaved(source,dir.path()));QTRY_VERIFY_WITH_TIMEOUT(observed!=0,10000);
    QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,5000);QVERIFY(!done[0][0].toBool());QVERIFY(!backend.report().value("inference_started").toBool());
    QTRY_VERIFY_WITH_TIMEOUT([&]{const auto h=OpenProcess(SYNCHRONIZE,FALSE,observed);if(!h)return true;const auto ended=WaitForSingleObject(h,0)==WAIT_OBJECT_0;CloseHandle(h);return ended;}(),5000);
    const auto unchanged=load_geometry_glb(native(source));QCOMPARE(unchanged.tris.size(),faces.size());QCOMPARE(unchanged.positions.size(),vertices.size());
    QTest::qWait(100);done.clear();QVERIFY(backend.openSaved(tetra(dir),dir.path()));QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,45000);
    QVERIFY2(done[0][0].toBool(),qPrintable(done[0][1].toString()));
#endif
  }
};
int main(int argc,char** argv){
  qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
  Image3DTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "test_image3d.moc"
