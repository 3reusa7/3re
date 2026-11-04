#include "texture_visibility_engine.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "backends/image_input.hpp"
#include "backends/image_material.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QElapsedTimer>
#include <iostream>
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 if(argc!=6)throw std::runtime_error("Usage: saved-atlas.glb|--unwrap source.png new-output.glb 1024|2048 original-topology.glb");
 auto path=std::filesystem::path(QString::fromLocal8Bit(argv[1]).toStdWString());
 const auto logical=p3dasset::load_geometry_glb(std::filesystem::path(QString::fromLocal8Bit(argv[5]).toStdWString()));if(std::string(argv[1])=="--source-hd"){
  auto source=p3dasset::prepare_image(QString::fromLocal8Bit(argv[2]));if(!source.error.isEmpty())throw std::runtime_error(source.error.toStdString());
  auto projected=p3dasset::project_source_material(logical,source.appearance,2048,false);p3dasset::save_source_material(*projected,QString::fromLocal8Bit(argv[3]));std::cout<<"HD_SOURCE "<<projected->texture->width()<<"x"<<projected->texture->height()<<" original detail, no inference"<<std::endl;return 0;
 }auto atlas=std::string(argv[1])=="--unwrap" ? std::make_shared<pixel3d::TexturedMesh>(p3dasset::create_source_atlas(logical,std::stoi(argv[4]))) : p3dasset::load_generated_appearance(path);if(!atlas)throw std::runtime_error("Existing atlas required");
 if(std::string(argv[1])=="--unwrap"){
  auto cache=*atlas;auto blank=std::make_shared<pixel3d::ImageRGBA>(1,1);blank->at(0,0)={128,128,128,255};cache.texture=blank;pixel3d::ExportOptions opt;opt.center_bottom=false;opt.units=pixel3d::ExportUnits::Pixels;opt.generator="Pixel3D UV diagnostic cache, no inferred appearance";auto bytes=pixel3d::write_glb(cache,opt);
  QFile f(QString::fromLocal8Bit(argv[3])+".atlas.glb");if(!f.open(QIODevice::WriteOnly|QIODevice::NewOnly)||f.write(reinterpret_cast<const char*>(bytes.data()),qint64(bytes.size()))!=qint64(bytes.size()))throw std::runtime_error("Atlas cache failed");
 }
 QElapsedTimer timer;timer.start();auto result=p3dasset::bake_visible_source(logical,*atlas,QImage(QString::fromLocal8Bit(argv[2])),std::stoi(argv[4]),true);
 auto target=QString::fromLocal8Bit(argv[3]);p3dasset::save_visibility_bake(result,logical,target);result.evidence["runtime_ms"]=timer.elapsed();
 QImage map(result.mesh.texture->width(),result.mesh.texture->height(),QImage::Format_RGBA8888);for(int y=0;y<map.height();++y)for(int x=0;x<map.width();++x){auto p=result.mesh.texture->at(x,y);map.setPixelColor(x,y,QColor(p.r,p.g,p.b,p.a));}
 if(!map.save(target+".base-color.png")||!result.provenance.save(target+".provenance.png"))throw std::runtime_error("Map save failed");QFile report(target+".bake.json");if(!report.open(QIODevice::WriteOnly|QIODevice::NewOnly))throw std::runtime_error("Report exists");auto json=result.evidence.dump(2);report.write(json.data(),qint64(json.size()));std::cout<<json<<std::endl;return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}

