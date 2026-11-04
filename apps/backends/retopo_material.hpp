#pragma once
// Retopology-only material preservation. No changes to the shared GLB writer.
#include <cstring>
#include <fstream>
#include "retopo_engine.hpp"
#include "auto_texture.hpp"

namespace p3dasset::retopo {
struct GlbParts { nlohmann::json doc; std::vector<uint8_t> bin; };
inline GlbParts split_glb(const std::vector<uint8_t>& bytes) {
  const auto word = [&](size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("Truncated GLB");
    uint32_t v; std::memcpy(&v, bytes.data() + offset, 4); return v;
  };
  if (bytes.size() < 28 || word(0) != 0x46546c67 || word(4) != 2 || word(8) != bytes.size() || word(16) != 0x4e4f534a)
    throw std::runtime_error("Invalid GLB container");
  const size_t json_size = word(12), off = 20 + json_size;
  if (off + 8 > bytes.size() || word(off + 4) != 0x004e4942 || off + 8 + word(off) != bytes.size())
    throw std::runtime_error("Invalid GLB binary chunk");
  return {nlohmann::json::parse(bytes.begin()+20, bytes.begin()+off), {bytes.begin()+off+8, bytes.end()}};
}
inline GlbParts read_material_glb(const std::filesystem::path& path) {
  const auto size = std::filesystem::file_size(path);
  if (size > 512ull*1024*1024) throw std::runtime_error("Material GLB exceeds 512 MiB bounded reader");
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> bytes(size);
  if (!f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) throw std::runtime_error("Cannot read material GLB");
  return split_glb(bytes);
}
inline std::vector<uint8_t> join_glb(GlbParts p) {
  while(p.bin.size()%4) p.bin.push_back(0);
  p.doc["buffers"] = {{{"byteLength", p.bin.size()}}};
  std::string js=p.doc.dump(); while(js.size()%4) js+=' ';
  if (js.size()+p.bin.size()+28 > UINT32_MAX) throw std::runtime_error("GLB exceeds size limit");
  std::vector<uint8_t> out;
  const auto word=[&](uint32_t v){ const auto* b=reinterpret_cast<const uint8_t*>(&v); out.insert(out.end(),b,b+4); };
  word(0x46546c67);word(2);word(uint32_t(js.size()+p.bin.size()+28));word(uint32_t(js.size()));word(0x4e4f534a);
  out.insert(out.end(),js.begin(),js.end());word(uint32_t(p.bin.size()));word(0x004e4942);out.insert(out.end(),p.bin.begin(),p.bin.end());return out;
}
inline nlohmann::json source_material(const GlbParts& p) {
  if(p.doc.at("meshes").size()!=1 || p.doc.at("meshes")[0].at("primitives").size()!=1)
    throw std::runtime_error("Multiple material primitives require a verified transfer path");
  auto m=p.doc.at("materials").at(p.doc["meshes"][0]["primitives"][0].at("material").get<size_t>());
  for (const auto* key : {"normalTexture","occlusionTexture","emissiveTexture"})
    if(m.contains(key)) throw std::runtime_error("Normal, occlusion and emissive transfer is not verified; source preserved");
  return m;
}
inline std::shared_ptr<const pixel3d::ImageRGBA> material_map(const GlbParts& p, const nlohmann::json& info) {
  if(info.value("texCoord",0)!=0 || info.contains("extensions")) throw std::runtime_error("Unsupported material UV mapping");
  const auto& tx=p.doc.at("textures").at(info.at("index").get<size_t>());
  const auto& img=p.doc.at("images").at(tx.at("source").get<size_t>());
  const auto& v=p.doc.at("bufferViews").at(img.at("bufferView").get<size_t>());
  const size_t off=v.value("byteOffset",size_t(0)), size=v.at("byteLength");
  if(v.value("buffer",0)!=0 || off>p.bin.size() || size>p.bin.size()-off) throw std::runtime_error("Invalid material image span");
  auto decoded=pixel3d::decode_image(std::span<const uint8_t>(p.bin.data()+off,size));
  if(!decoded) throw std::runtime_error(decoded.error().text);
  return std::make_shared<pixel3d::ImageRGBA>(std::move(decoded.value()));
}
inline std::vector<uint8_t> preserve_material(std::vector<uint8_t> bytes, nlohmann::json material,
                                             const pixel3d::ImageRGBA* mr, bool nearest = false, nlohmann::json base_sampler = {}, nlohmann::json mr_sampler = {}) {
  auto out=split_glb(bytes);
  material["pbrMetallicRoughness"]["baseColorTexture"]={{"index",0}};
  if(nearest) {out.doc["samplers"][0]["magFilter"]=9728;out.doc["samplers"][0]["minFilter"]=9728;}
  const auto keep_filter = [](nlohmann::json& dst, const nlohmann::json& src) {
    for(const auto* key : {"minFilter","magFilter"})
      if(src.contains(key) && src[key].get<int>() >= 0) dst[key]=src[key];
  };
  keep_filter(out.doc["samplers"][0],base_sampler);
  if(mr) {
    while(out.bin.size()%4)out.bin.push_back(0);
    const auto png=pixel3d::encode_png(*mr);
    const size_t view=out.doc["bufferViews"].size(), image=out.doc["images"].size(), texture=out.doc["textures"].size();
    out.doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",out.bin.size()},{"byteLength",png.size()}});
    out.bin.insert(out.bin.end(),png.begin(),png.end());
    out.doc["images"].push_back({{"bufferView",view},{"mimeType","image/png"},{"name","metallic_roughness"}});
    out.doc["samplers"].push_back({{"magFilter",9729},{"minFilter",9729},{"wrapS",33071},{"wrapT",33071}});
    keep_filter(out.doc["samplers"].back(),mr_sampler);
    out.doc["textures"].push_back({{"sampler",out.doc["samplers"].size()-1},{"source",image}});
    material["pbrMetallicRoughness"]["metallicRoughnessTexture"]={{"index",texture}};
  } else if(material["pbrMetallicRoughness"].contains("metallicRoughnessTexture")) {
    throw std::runtime_error("Refusing to drop metallic-roughness texture");
  }
  out.doc["materials"]={material};
  if(material.contains("extensions")) for(auto it=material["extensions"].begin();it!=material["extensions"].end();++it)
    out.doc["extensionsUsed"].push_back(it.key());
  return join_glb(std::move(out));
}
} // namespace p3dasset::retopo
