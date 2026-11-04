#include "image_material.hpp"
#include "image_input.hpp"
#include <QSaveFile>
#include <QFileInfo>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/io/glb_check.hpp"
namespace p3dasset {
std::shared_ptr<const pixel3d::TexturedMesh> project_source_material(const pixel3d::Mesh& mesh,const QImage& image,int texture_size,bool crisp){
  if(mesh.positions.empty() || mesh.tris.empty() || image.isNull())throw std::runtime_error("No mesh/source for appearance projection");
  int x0=image.width(),y0=image.height(),x1=-1,y1=-1;
  for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y).alpha()>=128){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);}
  if(x1<0)throw std::runtime_error("Source appearance is fully transparent");
  const int cap=std::clamp(texture_size,64,2048);
  auto img=image.copy(QRect(x0,y0,x1-x0+1,y1-y0+1));
  img=img.scaled(bounded_image_size(img.size(),cap),Qt::IgnoreAspectRatio,crisp?Qt::FastTransformation:Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
  // Extend foreground color into transparent texels, preserving valid pixels.
  // This avoids black/white bleeding while keeping the geometry fully opaque.
  std::vector<int> nearest(size_t(img.width())*img.height(),-1),queue;queue.reserve(nearest.size());
  for(int y=0;y<img.height();++y)for(int x=0;x<img.width();++x)if(img.constScanLine(y)[4*x+3]>=128){const int i=y*img.width()+x;nearest[size_t(i)]=i;queue.push_back(i);}
  for(size_t n=0;n<queue.size();++n){const int i=queue[n],x=i%img.width(),y=i/img.width();auto add=[&](int j){if(nearest[size_t(j)]<0){nearest[size_t(j)]=nearest[size_t(i)];queue.push_back(j);}};if(x)add(i-1);if(y)add(i-img.width());if(x+1<img.width())add(i+1);if(y+1<img.height())add(i+img.width());}
  auto texture=std::make_shared<pixel3d::ImageRGBA>(img.width(),img.height());
  for(int y=0;y<img.height();++y)for(int x=0;x<img.width();++x){const auto i=nearest[size_t(y*img.width()+x)];const auto c=img.pixelColor(i%img.width(),i/img.width());texture->at(x,y)={uint8_t(c.red()),uint8_t(c.green()),uint8_t(c.blue()),255};}
  auto out=std::make_shared<pixel3d::TexturedMesh>();out->positions=mesh.positions;out->normals.resize(mesh.positions.size());out->texture=texture;
  auto lo=mesh.positions.front(),hi=lo;
  for(auto p:mesh.positions){lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
  for(auto p:mesh.positions)out->uvs.push_back({std::clamp((p.x-lo.x)/std::max(1e-12,hi.x-lo.x),0.,1.),std::clamp((hi.y-p.y)/std::max(1e-12,hi.y-lo.y),0.,1.)});
  for(auto t:mesh.tris){const auto normal=pixel3d::cross(mesh.positions[t[1]]-mesh.positions[t[0]],mesh.positions[t[2]]-mesh.positions[t[0]]);for(auto i:t){out->indices.push_back(i);out->normals[i]=out->normals[i]+normal;}}
  for(auto& n:out->normals){n=pixel3d::normalize(n);if(pixel3d::length(n)<1e-12)n={0,0,1};}
  return out;
}
void save_source_material(const pixel3d::TexturedMesh& mesh,const QString& file){
  pixel3d::ExportOptions options;options.center_bottom=false;options.units=pixel3d::ExportUnits::Pixels;
  options.generator="Pixel3D TRELLIS Preview - approximate XY single-image projection";
  const auto bytes=pixel3d::write_glb(mesh,options);
  const auto check=pixel3d::check_glb_roundtrip(bytes,mesh,options,std::nullopt);
  if(!check.ok)throw std::runtime_error("Textured GLB round-trip validation failed");
  if(QFileInfo::exists(file))throw std::runtime_error("Refusing to overwrite an appearance asset");
  QSaveFile out(file);out.setDirectWriteFallback(false);
  if(!out.open(QIODevice::WriteOnly) || out.write(reinterpret_cast<const char*>(bytes.data()),qint64(bytes.size()))!=qint64(bytes.size()) || !out.commit())throw std::runtime_error("Could not commit textured GLB");
}
}
