#include "texture_visibility_engine.hpp"
#include "xatlas.h"
#include <iostream>
#include <cmath>
#include <memory>
#include <stdexcept>
namespace p3dasset {
pixel3d::TexturedMesh create_source_atlas(const pixel3d::Mesh& m,int size){
 using namespace pixel3d;
 if((size!=1024&&size!=2048)||m.positions.empty()||m.positions.size()>150000||m.tris.empty()||m.tris.size()>200000)throw std::runtime_error("Atlas bounded profile exceeded");
 std::vector<float> points;std::vector<uint32_t> indices;std::vector<vec3d> normals(m.positions.size());
 // xatlas uses an absolute FLT_EPSILON area cutoff. Metric-sized detailed
 // meshes otherwise lose valid tiny faces. Rescale parameterization input only
 // by an exact power of two; exported positions always use original xref data.
 auto lo=m.positions.front(),hi=lo;for(auto p:m.positions){lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
 double diag=length(hi-lo);if(!std::isfinite(diag)||diag<=0)throw std::runtime_error("Invalid atlas extent");
 double scale=std::exp2(std::floor(std::log2(1024./diag)));std::cout<<"ATLAS parameterization-only power-of-two scale "<<scale<<std::endl;
 for(auto p:m.positions){for(double v:{p.x,p.y,p.z}){float f=float(v*scale);if(!std::isfinite(f))throw std::runtime_error("Atlas range overflow");points.push_back(f);}}
 for(auto t:m.tris){for(auto i:t)if(i>=m.positions.size())throw std::runtime_error("Invalid atlas input index");auto n=cross(m.positions[t[1]]-m.positions[t[0]],m.positions[t[2]]-m.positions[t[0]]);for(auto i:t){indices.push_back(i);normals[i]=normals[i]+n;}}
 std::unique_ptr<xatlas::Atlas,decltype(&xatlas::Destroy)> a(xatlas::Create(),xatlas::Destroy);
 xatlas::SetProgressCallback(a.get(),[](xatlas::ProgressCategory c,int p,void*){if(p%25==0)std::cout<<"ATLAS "<<xatlas::StringForEnum(c)<<" "<<p<<"%"<<std::endl;return true;});
 xatlas::MeshDecl d;d.vertexPositionData=points.data();d.vertexPositionStride=12;d.vertexCount=uint32_t(m.positions.size());d.indexData=indices.data();d.indexCount=uint32_t(indices.size());d.indexFormat=xatlas::IndexFormat::UInt32;d.epsilon=0;
 if(xatlas::AddMesh(a.get(),d)!=xatlas::AddMeshError::Success)throw std::runtime_error("xatlas rejected input; geometry unchanged");
 xatlas::PackOptions pack;pack.resolution=uint32_t(size);pack.padding=6;pack.bilinear=true;pack.blockAlign=true;
 xatlas::Generate(a.get(),xatlas::ChartOptions(),pack);
 if(a->meshCount!=1||a->atlasCount!=1||!a->width||!a->height)throw std::runtime_error("Atlas failed single-sheet contract");
 const auto& mesh=a->meshes[0];if(mesh.indexCount!=indices.size())throw std::runtime_error("Atlas changed face count");
 TexturedMesh out;for(uint32_t i=0;i<mesh.vertexCount;++i){auto v=mesh.vertexArray[i];if(v.xref>=m.positions.size())throw std::runtime_error("Atlas invalid source reference");out.positions.push_back(m.positions[v.xref]);out.normals.push_back(normalize(normals[v.xref]));out.uvs.push_back({double(v.uv[0])/a->width,double(v.uv[1])/a->height});}
 out.indices.assign(mesh.indexArray,mesh.indexArray+mesh.indexCount);return out;
}
}
