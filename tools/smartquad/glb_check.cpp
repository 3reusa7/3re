// Independent static GLB export inspection. Does not modify input or geometry.
#include <nlohmann/json.hpp>
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <stb_image.h>
#include <tiny_gltf.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using nlohmann::json;
namespace {
void need(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
const tinygltf::Accessor& accessor(const tinygltf::Model& m, int id) {
  need(id>=0 && static_cast<size_t>(id)<m.accessors.size(), "Invalid accessor index");
  const auto& a=m.accessors[static_cast<size_t>(id)];
  need(!a.sparse.isSparse, "Sparse accessor unsupported by independent checker");
  need(a.bufferView>=0 && static_cast<size_t>(a.bufferView)<m.bufferViews.size(), "Invalid bufferView");
  const auto& v=m.bufferViews[static_cast<size_t>(a.bufferView)];
  need(v.buffer>=0 && static_cast<size_t>(v.buffer)<m.buffers.size(), "Invalid buffer");
  const auto stride=a.ByteStride(v);
  need(stride>0, "Invalid accessor stride");
  const auto element=static_cast<size_t>(tinygltf::GetComponentSizeInBytes(a.componentType)*tinygltf::GetNumComponentsInType(a.type));
  need(static_cast<size_t>(stride)>=element, "Accessor stride is too small");
  const size_t bytes=a.count ? (a.count-1)*static_cast<size_t>(stride)+element : 0;
  need(a.byteOffset<=v.byteLength && bytes<=v.byteLength-a.byteOffset, "Accessor outside bufferView");
  need(v.byteOffset<=m.buffers[static_cast<size_t>(v.buffer)].data.size() &&
       v.byteLength<=m.buffers[static_cast<size_t>(v.buffer)].data.size()-v.byteOffset, "BufferView outside buffer");
  return a;
}
const unsigned char* data(const tinygltf::Model& m,const tinygltf::Accessor& a,size_t i) {
  const auto& v=m.bufferViews[static_cast<size_t>(a.bufferView)];
  return m.buffers[static_cast<size_t>(v.buffer)].data.data()+v.byteOffset+a.byteOffset+i*static_cast<size_t>(a.ByteStride(v));
}
uint32_t index(const unsigned char* p,int type) {
  if(type==TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) return *p;
  if(type==TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) { uint16_t v;std::memcpy(&v,p,2);return v; }
  need(type==TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,"Unsupported index type");
  uint32_t v;std::memcpy(&v,p,4);return v;
}
void texture(const tinygltf::Model& m,int id) {
  if(id<0) return;
  need(static_cast<size_t>(id)<m.textures.size(),"Invalid texture reference");
  const int image=m.textures[static_cast<size_t>(id)].source;
  need(image>=0 && static_cast<size_t>(image)<m.images.size(),"Invalid image reference");
  const auto& im=m.images[static_cast<size_t>(image)];
  need(im.width>0 && im.height>0 && !im.image.empty(),"Texture could not be decoded");
}
}
int main(int argc,char** argv) {
  json report={{"ok",false}};
  try {
    need(argc==3,"Usage: pixel3d-smartquad-check asset.glb fresh-report.json");
    need(!std::filesystem::exists(argv[2]),"Refusing report overwrite");
    tinygltf::Model m; tinygltf::TinyGLTF loader;std::string error,warning;
    need(loader.LoadBinaryFromFile(&m,&error,&warning,argv[1]),"GLB could not be independently reloaded");
    need(m.skins.empty() && m.animations.empty(),"Static mesh only: skins/animations are unsupported");
    size_t triangles=0,vertices=0,primitives=0;
    json materials=json::array();
    for(const auto& mat:m.materials) {
      texture(m,mat.pbrMetallicRoughness.baseColorTexture.index);
      texture(m,mat.pbrMetallicRoughness.metallicRoughnessTexture.index);
      texture(m,mat.normalTexture.index);texture(m,mat.occlusionTexture.index);texture(m,mat.emissiveTexture.index);
      materials.push_back({{"name",mat.name},{"base_color_texture",mat.pbrMetallicRoughness.baseColorTexture.index},
        {"metallic_roughness_texture",mat.pbrMetallicRoughness.metallicRoughnessTexture.index},
        {"normal_texture",mat.normalTexture.index},{"emissive_texture",mat.emissiveTexture.index},
        {"occlusion_texture",mat.occlusionTexture.index},{"alpha_mode",mat.alphaMode},{"double_sided",mat.doubleSided}});
    }
    for(const auto& mesh:m.meshes) for(const auto& p:mesh.primitives) {
      need(p.mode==TINYGLTF_MODE_TRIANGLES,"Game GLB must contain triangles");
      need(p.attributes.contains("POSITION"),"Missing POSITION");
      const auto& pos=accessor(m,p.attributes.at("POSITION"));
      need(pos.type==TINYGLTF_TYPE_VEC3 && pos.componentType==TINYGLTF_COMPONENT_TYPE_FLOAT,"Invalid position layout");
      need(pos.count>0 && pos.count<=1000000,"Invalid vertex count");
      for(size_t i=0;i<pos.count;++i) for(int c=0;c<3;++c) {
        float value;std::memcpy(&value,data(m,pos,i)+4*c,4);need(std::isfinite(value),"Nonfinite vertex");
      }
      const auto& ids=accessor(m,p.indices);
      need(ids.type==TINYGLTF_TYPE_SCALAR && ids.count%3==0,"Invalid triangle index layout");
      for(size_t i=0;i<ids.count;++i) need(index(data(m,ids,i),ids.componentType)<pos.count,"Triangle index out of range");
      need(p.attributes.contains("NORMAL"),"Missing NORMAL");
      for(const auto& [name,id]:p.attributes) {
        const auto& a=accessor(m,id);need(a.count==pos.count,"Attribute vertex counts differ");
        if(a.componentType==TINYGLTF_COMPONENT_TYPE_FLOAT) {
          const int count=tinygltf::GetNumComponentsInType(a.type);
          for(size_t i=0;i<a.count;++i) for(int c=0;c<count;++c) {
            float value;std::memcpy(&value,data(m,a,i)+4*c,4);need(std::isfinite(value),"Nonfinite vertex attribute");
          }
        }
      }
      need(p.material>=0 && static_cast<size_t>(p.material)<m.materials.size(),"Missing/invalid material");
      const auto& mat=m.materials[static_cast<size_t>(p.material)];
      if(mat.pbrMetallicRoughness.baseColorTexture.index>=0 || mat.normalTexture.index>=0)
        need(p.attributes.contains("TEXCOORD_0"),"Textured primitive missing UVs");
      triangles+=ids.count/3; vertices+=pos.count; ++primitives;
    }
    need(triangles>0 && primitives>0,"Empty exported GLB");
    report={{"ok",true},{"triangles",triangles},{"vertices",vertices},{"primitives",primitives},
      {"materials",materials},{"images",m.images.size()},{"warnings",warning},
      {"scope","Independent TinyGLTF decode, accessor bounds, actual serialized geometry/attributes and material texture references. Topology and geometry are separately audited."}};
  } catch(const std::exception& e) { report["error"]=e.what(); }
  if(argc==3 && !std::filesystem::exists(argv[2])) { std::ofstream out(argv[2]);out<<report.dump(2)<<'\n'; }
  std::cout<<report.dump()<<'\n';return report.value("ok",false)?0:1;
}
