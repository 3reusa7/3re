#include "mesh_asset.hpp"
#define TINYGLTF_NO_INCLUDE_JSON
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <stb_image.h>
#include <tiny_gltf.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <stdexcept>
#include "pixel3d/mesh/topology.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dasset {
namespace {
// Character textures are baked up to 4096 (3re 2.3): one 4096 RGBA map decodes to 64 MiB.
constexpr int kMaxTextureSide = 4096;
constexpr uint64_t kMaxDecodedImageBytes = 96ull << 20;

// Raw 512 DC meshes can exceed two million faces before cleanup. Keep an
// explicit bounded recovery profile, shared with the isolated intersection tool.
constexpr size_t kMaxVertices = 2000000, kMaxTriangles = 4000000;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
const tinygltf::Accessor& accessor(const tinygltf::Model& model, int index) {
  require(index >= 0 && size_t(index) < model.accessors.size(), "Invalid accessor");
  const auto& a = model.accessors[size_t(index)];
  require(!a.sparse.isSparse && a.bufferView >= 0 && size_t(a.bufferView) < model.bufferViews.size(), "Unsupported accessor");
  const auto& view = model.bufferViews[size_t(a.bufferView)];
  require(view.buffer >= 0 && size_t(view.buffer) < model.buffers.size(), "Invalid buffer");
  const auto& bytes = model.buffers[size_t(view.buffer)].data;
  const int width = tinygltf::GetComponentSizeInBytes(a.componentType) * tinygltf::GetNumComponentsInType(a.type);
  const int stride = a.ByteStride(view);
  require(width > 0 && stride >= width, "Invalid attribute layout (type/stride)");
  require(a.count <= kMaxTriangles * 3, "Accessor exceeds bounded recovery profile (4 million triangles)");
  const auto component = tinygltf::GetComponentSizeInBytes(a.componentType);
  require(component > 0 && a.byteOffset % size_t(component) == 0 &&
          view.byteOffset % size_t(component) == 0, "Misaligned accessor offset");
  require(view.byteStride == 0 || (view.byteStride >= 4 && view.byteStride <= 252 && view.byteStride % 4 == 0),
          "Invalid interleaved attribute stride");
  require(view.byteOffset <= bytes.size() && view.byteLength <= bytes.size() - view.byteOffset, "Invalid buffer bounds");
  require(a.byteOffset <= view.byteLength, "Invalid accessor offset");
  const size_t length = a.count ? (a.count - 1) * size_t(stride) + size_t(width) : 0;
  require(length <= view.byteLength - a.byteOffset, "Invalid accessor bounds");
  return a;
}
const unsigned char* data(const tinygltf::Model& model, const tinygltf::Accessor& a, size_t i) {
  const auto& view = model.bufferViews[size_t(a.bufferView)];
  return model.buffers[size_t(view.buffer)].data.data() + view.byteOffset + a.byteOffset + i * size_t(a.ByteStride(view));
}
template<class T> T value(const unsigned char* p) { T result; std::memcpy(&result, p, sizeof(T)); return result; }
uint32_t index_value(const unsigned char* p, int type) {
  if (type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) return *p;
  if (type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) return value<uint16_t>(p);
  require(type == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, "Unsupported index type");
  return value<uint32_t>(p);
}
void validate(const pixel3d::Mesh& mesh) {
  require(!mesh.positions.empty() && !mesh.tris.empty() && mesh.positions.size() <= kMaxVertices && mesh.tris.size() <= kMaxTriangles, "Mesh exceeds bounded geometry profile");
  for (auto p : mesh.positions) require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Nonfinite position");
  for (auto face : mesh.tris) for (auto i : face) require(i < mesh.positions.size(), "Triangle index out of range");
}
tinygltf::Model read_model(const std::filesystem::path& path) {
  require(std::filesystem::file_size(path) <= 256ull * 1024 * 1024, "GLB exceeds bounded import size");
  tinygltf::Model model; tinygltf::TinyGLTF loader; std::string error, warning;
  require(loader.LoadBinaryFromFile(&model, &error, &warning, pixel3d::to_utf8(path)), "Could not decode GLB");
  require(model.skins.empty() && model.animations.empty() && model.meshes.size() == 1, "Expected one static generated mesh");
  require(model.nodes.size() == 1 && model.defaultScene >= 0 && size_t(model.defaultScene) < model.scenes.size() &&
          model.scenes[size_t(model.defaultScene)].nodes == std::vector<int>{0}, "Expected a single generated scene node");
  for (const auto& material : model.materials)
    require(material.normalTexture.index < 0 && material.occlusionTexture.index < 0 && material.emissiveTexture.index < 0,
            "Normal, occlusion and emissive maps are not supported by this preview");
  size_t mesh_nodes = 0;
  for (const auto& node : model.nodes) if (node.mesh >= 0) {
    ++mesh_nodes;
    require(node.mesh == 0 && node.matrix.empty() && node.translation.empty() && node.rotation.empty() && node.scale.empty(),
            "Generated mesh must have baked identity transforms");
  }
  require(mesh_nodes == 1, "Instanced generated meshes are not supported");
  require(model.meshes[0].primitives.size() == 1, "Expected one static generated primitive");
  return model;
}
pixel3d::Mesh geometry_from_accessors(const tinygltf::Model& model, int position_index, int index_index) {
  pixel3d::Mesh mesh;
  const auto& positions = accessor(model, position_index);
  require(positions.type == TINYGLTF_TYPE_VEC3 && positions.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
          !positions.normalized && positions.count <= kMaxVertices, "Invalid position layout/count");
  const auto& indices = accessor(model, index_index);
  require(indices.type == TINYGLTF_TYPE_SCALAR && !indices.normalized && indices.count % 3 == 0 &&
          indices.count / 3 <= kMaxTriangles, "Invalid triangles");
  require(model.bufferViews[size_t(indices.bufferView)].byteStride == 0, "Index buffer must be tightly packed");
  for (size_t i = 0; i < positions.count; ++i) {
    const auto p = data(model, positions, i);
    mesh.positions.push_back({value<float>(p), value<float>(p + 4), value<float>(p + 8)});
  }
  for (size_t i = 0; i < indices.count; i += 3) {
    std::array<uint32_t, 3> face{};
    for (int j = 0; j < 3; ++j) {
      const auto v = index_value(data(model, indices, i + size_t(j)), indices.componentType);
      require(v < positions.count, "Index outside primitive");
      face[size_t(j)] = v;
    }
    mesh.tris.push_back(face);
  }
  validate(mesh);
  return mesh;
}
pixel3d::Mesh render_geometry(const tinygltf::Model& model) {
  const auto& primitive = model.meshes[0].primitives[0];
  require(primitive.mode == TINYGLTF_MODE_TRIANGLES && primitive.targets.empty() && primitive.attributes.contains("POSITION"), "Expected static triangles");
  return geometry_from_accessors(model, primitive.attributes.at("POSITION"), primitive.indices);
}
pixel3d::Mesh logical_geometry(const tinygltf::Model& model, const pixel3d::Mesh& render) {
  if (!model.asset.extras.Has("pixel3d_topology")) return render;
  const auto& topology = model.asset.extras.Get("pixel3d_topology");
  require(topology.IsObject() && topology.Has("position_accessor") && topology.Has("index_accessor") &&
          topology.Get("position_accessor").IsInt() && topology.Get("index_accessor").IsInt(), "Invalid original topology references");
  auto logical = geometry_from_accessors(model, topology.Get("position_accessor").GetNumberAsInt(), topology.Get("index_accessor").GetNumberAsInt());
  require(geometrically_identical(logical, render), "Original topology does not match rendered triangles");
  return logical;
}
using TriangleKey = std::array<uint32_t, 9>;
std::vector<TriangleKey> triangle_keys(const pixel3d::Mesh& mesh) {
  validate(mesh);
  std::vector<TriangleKey> keys;
  keys.reserve(mesh.tris.size());
  for (const auto& tri : mesh.tris) {
    TriangleKey best{};
    for (int rotation = 0; rotation < 3; ++rotation) {
      TriangleKey current{};
      for (int k = 0; k < 3; ++k) {
        const auto p = mesh.positions[tri[size_t((k + rotation) % 3)]];
        const std::array<double, 3> xyz{p.x, p.y, p.z};
        for (int c = 0; c < 3; ++c) {
          const float f = float(xyz[size_t(c)]);
          require(std::isfinite(f), "Position exceeds F32 geometry range");
          current[size_t(3 * k + c)] = f == 0 ? 0 : std::bit_cast<uint32_t>(f);
        }
      }
      if (rotation == 0 || current < best) best = current;
    }
    keys.push_back(best);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}
const tinygltf::Image& checked_image(const tinygltf::Model& model, int texture_index) {
  require(texture_index >= 0 && size_t(texture_index) < model.textures.size(), "Missing material texture");
  const auto& texture = model.textures[size_t(texture_index)];
  require(texture.extensions.empty(), "Texture extensions are not supported by this preview");
  require(texture.source >= 0 && size_t(texture.source) < model.images.size(), "Missing material image");
  const auto& image = model.images[size_t(texture.source)];
  require(image.bufferView >= 0 && image.uri.empty(), "Generated material images must be embedded in the GLB");
  require(image.width > 0 && image.height > 0 && image.width <= kMaxTextureSide && image.height <= kMaxTextureSide && image.bits == 8 &&
          image.component >= 3 && image.component <= 4 && image.image.size() == size_t(image.width) * image.height * image.component,
          "Unsupported appearance image layout");
  return image;
}
nlohmann::json texture_info(const tinygltf::Model& model, const tinygltf::TextureInfo& info, bool allow_mipmap_filter) {
  require(info.texCoord == 0 && info.extensions.empty(), "Only untransformed TEXCOORD_0 material maps are supported");
  const auto& image = checked_image(model, info.index);
  const auto& texture = model.textures[size_t(info.index)];
  require(texture.sampler >= -1, "Invalid material sampler");
  tinygltf::Sampler sampler;
  if (texture.sampler >= 0) {
    require(size_t(texture.sampler) < model.samplers.size(), "Invalid material sampler");
    sampler = model.samplers[size_t(texture.sampler)];
  }
  require(sampler.extensions.empty() && (sampler.minFilter == -1 || sampler.minFilter == 9728 || sampler.minFilter == 9729 || (allow_mipmap_filter && sampler.minFilter >= 9984 && sampler.minFilter <= 9987)) &&
          (sampler.magFilter == -1 || sampler.magFilter == 9728 || sampler.magFilter == 9729),
          "Unsupported material filtering; only default, NEAREST and LINEAR are supported");
  require((sampler.wrapS == 33071 || sampler.wrapS == 10497 || sampler.wrapS == 33648) &&
          (sampler.wrapT == 33071 || sampler.wrapT == 10497 || sampler.wrapT == 33648), "Invalid material wrap mode");
  return {{"texture_index", info.index}, {"image_name", image.name}, {"size", {image.width, image.height}}, {"texcoord", info.texCoord},
          {"sampler", {{"minFilter", sampler.minFilter}, {"magFilter", sampler.magFilter}, {"wrapS", sampler.wrapS}, {"wrapT", sampler.wrapT}}},
          {"preview_filter_approximate", sampler.minFilter != 9728 || sampler.magFilter != 9728},
          {"preview_wrap_approximate", sampler.wrapS != 33071 || sampler.wrapT != 33071}};
}
nlohmann::json describe_material(const tinygltf::Model& model, bool allow_mipmap_filter) {
  const auto& primitive = model.meshes[0].primitives[0];
  if (model.images.empty() && model.textures.empty()) return {{"kind", "geometry_only"}, {"maps", nlohmann::json::object()}};
  require(primitive.material >= 0 && size_t(primitive.material) < model.materials.size() && primitive.attributes.contains("TEXCOORD_0"), "Incomplete saved appearance");
  require(!primitive.attributes.contains("COLOR_0"), "Vertex color modulation is not supported by this preview");
  const auto& material = model.materials[size_t(primitive.material)];
  const auto& pbr = material.pbrMetallicRoughness;
  const bool own_auto_texture = model.asset.extras.Has("pixel3d_appearance") && model.asset.extras.Get("pixel3d_appearance").Has("kind") &&
                                model.asset.extras.Get("pixel3d_appearance").Get("kind").IsString() &&
                                model.asset.extras.Get("pixel3d_appearance").Get("kind").Get<std::string>() == "auto_texture";
  require(material.alphaMode == "OPAQUE" || (own_auto_texture && material.alphaMode == "BLEND"), "Only opaque generated material previews are supported");
  require(pbr.baseColorFactor == std::vector<double>({1, 1, 1, 1}), "Non-neutral base-color factors are not supported by this preview");
  require(std::isfinite(pbr.metallicFactor) && pbr.metallicFactor >= 0 && pbr.metallicFactor <= 1 &&
          std::isfinite(pbr.roughnessFactor) && pbr.roughnessFactor >= 0 && pbr.roughnessFactor <= 1, "Invalid PBR factors");
  require(material.emissiveFactor == std::vector<double>({0, 0, 0}) && pbr.extensions.empty(), "Emissive or extended PBR materials are not supported by this preview");
  for (const auto& [name, unused] : material.extensions)
    require(name == "KHR_materials_unlit", "Unsupported material extension");
  std::string kind = "unknown";
  if(model.asset.generator == "Pixel3D TRELLIS Preview - approximate XY single-image projection")kind="source_projected";
  if (model.asset.extras.Has("pixel3d_appearance")) {
    const auto& appearance = model.asset.extras.Get("pixel3d_appearance");
    require(appearance.IsObject() && appearance.Has("kind") && appearance.Get("kind").IsString(), "Invalid appearance provenance");
    kind = appearance.Get("kind").Get<std::string>();
  }
  nlohmann::json maps{{"base_color", texture_info(model, pbr.baseColorTexture, allow_mipmap_filter)}};
  if (pbr.metallicRoughnessTexture.index >= 0) maps["metallic_roughness"] = texture_info(model, pbr.metallicRoughnessTexture, allow_mipmap_filter);
  return {{"kind", kind}, {"name", material.name}, {"maps", maps}, {"base_color_factor", pbr.baseColorFactor},
          {"metallic_factor", pbr.metallicFactor}, {"roughness_factor", pbr.roughnessFactor}, {"alpha_mode", material.alphaMode},
          {"double_sided", material.doubleSided}, {"unlit", material.extensions.contains("KHR_materials_unlit")},
          {"native_preview", "base_color_only"}, {"preview_filter_approximate", maps["base_color"]["preview_filter_approximate"]},
          {"preview_wrap_approximate", maps["base_color"]["preview_wrap_approximate"]},
          {"preview_double_sided_approximate", material.doubleSided}, {"preview_alpha_approximate", material.alphaMode != "OPAQUE"}};
}
}  // namespace

SavedPreviewMemory saved_preview_memory(const std::filesystem::path& path) {
  constexpr uint64_t MiB=1024ull*1024, GiB=1024*MiB;
  SavedPreviewMemory budget;
  budget.encoded_bytes=std::filesystem::file_size(path);
  // Larger/unknown assets retain the existing conservative admission. The small
  // profile is deliberately limited to the static native generated-asset path.
  if(budget.encoded_bytes>16*MiB)return budget;
  std::ifstream input(path,std::ios::binary);
  std::array<uint32_t,5> header{};
  require(bool(input.read(reinterpret_cast<char*>(header.data()),20)),"Truncated GLB header");
  require(header[0]==0x46546c67 && header[1]==2 && header[2]==budget.encoded_bytes &&
          header[4]==0x4e4f534a && header[3]%4==0 && header[3]<=budget.encoded_bytes-20,"Invalid GLB header/chunk bounds");
  if(header[3]>256*1024)return budget;
  std::string text(header[3],' ');
  require(bool(input.read(text.data(),std::streamsize(text.size()))),"Truncated GLB JSON");
  const auto j=nlohmann::json::parse(text);
  require(j.is_object(),"Invalid GLB metadata");
  // Reject external and data URI payloads before TinyGLTF can allocate/read them.
  for(const auto* key:{"buffers","images"})if(j.contains(key)){
    require(j[key].is_array(),"Invalid GLB resource list");
    for(const auto& resource:j[key])require(resource.is_object() && !resource.contains("uri"),"Saved preview requires embedded GLB resources");
  }
  if(!j.contains("buffers") || j["buffers"].size()!=1 ||
      (j.contains("images") && j["images"].size()>2) ||
      !j.contains("accessors") || j["accessors"].size()>16)return budget;
  std::array<uint32_t,2> bin{};
  require(bool(input.read(reinterpret_cast<char*>(bin.data()),8)) && bin[1]==0x004e4942 && bin[0]%4==0 &&
      uint64_t(bin[0])+28+header[3]==budget.encoded_bytes,"Invalid GLB BIN chunk");
  auto integer=[](const nlohmann::json& value){
    require(value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>()>=0),"Invalid GLB unsigned metadata");
    return value.get<uint64_t>();
  };
  const auto declared=integer(j["buffers"][0].at("byteLength"));
  require(declared<=bin[0] && bin[0]-declared<=3,"Invalid embedded buffer length");
  const auto views=j.value("bufferViews",nlohmann::json::array());require(views.is_array(),"Invalid buffer views");
  // TinyGLTF's image callback receives a pointer plus an int length. Validate
  // image-only views too, before any STB header read, using actual BIN bounds.
  for(const auto& view:views){
    require(view.is_object() && integer(view.at("buffer"))==0,"Invalid embedded buffer view");
    const auto offset=integer(view.value("byteOffset",nlohmann::json(0)));
    const auto length=integer(view.at("byteLength"));
    require(offset<=declared && length<=declared-offset,"Buffer view exceeds embedded BIN data");
  }
  for(const auto& image:j.value("images",nlohmann::json::array())){
    const auto index=integer(image.at("bufferView"));require(index<views.size(),"Invalid embedded image view");
    const auto length=integer(views[size_t(index)].at("byteLength"));
    require(length>0 && length<=uint64_t(std::numeric_limits<int>::max()),"Invalid encoded image length");
  }
  // TinyGLTF validates the BIN/accessor layout; the callback only reads image
  // headers, including unused images. No decoded pixel buffer is allocated.
  tinygltf::TinyGLTF loader; tinygltf::Model model;std::string error,warning;
  loader.SetImageLoader([&](tinygltf::Image* image,int,std::string*,std::string*,int,int,
                            const unsigned char* bytes,int size,void*){
    int w=0,h=0,channels=0;
    if(!stbi_info_from_memory(bytes,size,&w,&h,&channels) || w<=0 || h<=0 || w>kMaxTextureSide || h>kMaxTextureSide ||
        channels<1 || channels>4 || stbi_is_16_bit_from_memory(bytes,size))return false;
    const auto decoded=uint64_t(w)*uint64_t(h)*4;
    if(decoded>kMaxDecodedImageBytes || budget.decoded_image_bytes>kMaxDecodedImageBytes-decoded)return false;
    budget.decoded_image_bytes+=decoded;
    image->width=w;image->height=h;image->component=4;image->bits=8;
    image->pixel_type=TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;return true;
  },nullptr);
  std::vector<unsigned char> bytes(size_t(budget.encoded_bytes));input.clear();input.seekg(0);
  require(bool(input.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))),"Could not read bounded GLB");
  require(loader.LoadBinaryFromMemory(&model,&error,&warning,bytes.data(),unsigned(bytes.size())),"Invalid GLB or oversized/unsupported image header");
  if(model.meshes.size()!=1 || model.meshes[0].primitives.size()!=1 || !model.skins.empty() || !model.animations.empty())return budget;
  const auto& primitive=model.meshes[0].primitives[0];
  if(primitive.mode!=TINYGLTF_MODE_TRIANGLES || !primitive.targets.empty() || !primitive.attributes.contains("POSITION"))return budget;
  for(size_t i=0;i<model.accessors.size();++i){
    const auto& a=accessor(model,int(i));if(a.count>600000)return budget;
  }
  auto count=[&](int pi,int ii){
    const auto& p=accessor(model,pi);const auto& f=accessor(model,ii);
    require(p.type==TINYGLTF_TYPE_VEC3 && p.componentType==TINYGLTF_COMPONENT_TYPE_FLOAT &&
            !p.normalized && f.type==TINYGLTF_TYPE_SCALAR && !f.normalized && f.count%3==0,"Invalid geometry counts/layout");
    budget.vertices=std::max(budget.vertices,uint64_t(p.count));
    budget.triangles=std::max(budget.triangles,uint64_t(f.count/3));
  };
  count(primitive.attributes.at("POSITION"),primitive.indices);
  if(model.asset.extras.Has("pixel3d_topology")){
    const auto& t=model.asset.extras.Get("pixel3d_topology");
    require(t.IsObject() && t.Has("position_accessor") && t.Has("index_accessor") &&
      t.Get("position_accessor").IsInt() && t.Get("index_accessor").IsInt(),"Invalid original topology references");
    count(t.Get("position_accessor").GetNumberAsInt(),t.Get("index_accessor").GetNumberAsInt());
  }
  if(budget.vertices>150000 || budget.triangles>200000)return budget;
  // Conservative concurrent working headroom: BIN/parser copies, render and
  // logical mesh copies, topology/BVH map nodes and texture/preview copies.
  // Calibrated with bounded saved-asset measurements; not a universal peak claim.
  const uint64_t headroom=128*MiB+4*budget.encoded_bytes+256*budget.vertices+
      1600*budget.triangles+4*budget.decoded_image_bytes;
  budget.required_free_bytes=GiB+std::max(512*MiB,headroom);
  budget.preview_free_bytes=GiB+std::max(128*MiB,2*budget.encoded_bytes+
      128*budget.vertices+128*budget.triangles+4*budget.decoded_image_bytes);
  budget.bounded_profile=true;return budget;
}

bool geometrically_identical(const pixel3d::Mesh& a, const pixel3d::Mesh& b) {
  if (a.tris.size() != b.tris.size()) return false;
  try { return triangle_keys(a) == triangle_keys(b); } catch (const std::runtime_error&) { return false; }
}
pixel3d::Mesh load_geometry_glb(const std::filesystem::path& path) {
  const auto model = read_model(path);
  return logical_geometry(model, render_geometry(model));
}
nlohmann::json material_info(const std::filesystem::path& path, bool allow_mipmap_filter) {
  const auto model = read_model(path);
  const auto render = render_geometry(model);
  (void)logical_geometry(model, render);
  return describe_material(model, allow_mipmap_filter);
}

std::shared_ptr<const pixel3d::TexturedMesh> load_generated_appearance(const std::filesystem::path& path, bool allow_mipmap_filter) {
  const auto model=read_model(path);
  const auto mesh=render_geometry(model);
  (void)logical_geometry(model,mesh);
  if(model.images.empty() && model.textures.empty())return {};
  (void)describe_material(model, allow_mipmap_filter);
  require(model.meshes[0].primitives.size()==1,"Only a single generated appearance primitive is supported");
  const auto& primitive=model.meshes[0].primitives[0];
  require(primitive.material>=0 && size_t(primitive.material)<model.materials.size() && primitive.attributes.contains("TEXCOORD_0"),"Incomplete saved appearance");
  const auto texture=model.materials[size_t(primitive.material)].pbrMetallicRoughness.baseColorTexture.index;
  require(texture>=0 && size_t(texture)<model.textures.size(),"Missing base-color texture");
  const auto source=model.textures[size_t(texture)].source;
  require(source>=0 && size_t(source)<model.images.size(),"Missing base-color image");
  const auto& image=model.images[size_t(source)];
  (void)checked_image(model,texture);
  auto result=std::make_shared<pixel3d::TexturedMesh>();result->positions=mesh.positions;result->normals.resize(mesh.positions.size());
  const auto& uv=accessor(model,primitive.attributes.at("TEXCOORD_0"));
  require(uv.type==TINYGLTF_TYPE_VEC2 && uv.componentType==TINYGLTF_COMPONENT_TYPE_FLOAT && uv.count==mesh.positions.size(),"Invalid UV layout");
  for(size_t i=0;i<uv.count;++i){const auto p=data(model,uv,i);const auto u=value<float>(p),v=value<float>(p+4);require(std::isfinite(u)&&std::isfinite(v),"Nonfinite UV");result->uvs.push_back({u,v});}
  for(auto t:mesh.tris){const auto n=pixel3d::cross(mesh.positions[t[1]]-mesh.positions[t[0]],mesh.positions[t[2]]-mesh.positions[t[0]]);for(auto i:t){result->indices.push_back(i);result->normals[i]=result->normals[i]+n;}}
  if(primitive.attributes.contains("NORMAL")){
    const auto& normals=accessor(model,primitive.attributes.at("NORMAL"));
    require(normals.type==TINYGLTF_TYPE_VEC3 && normals.componentType==TINYGLTF_COMPONENT_TYPE_FLOAT && normals.count==mesh.positions.size(),"Invalid normal layout");
    for(size_t i=0;i<normals.count;++i){const auto p=data(model,normals,i);const pixel3d::vec3d n{value<float>(p),value<float>(p+4),value<float>(p+8)};
      require(std::isfinite(n.x)&&std::isfinite(n.y)&&std::isfinite(n.z)&&std::abs(pixel3d::length(n)-1)<.005,"Invalid saved normal");result->normals[i]=n;}
  }else for(auto& n:result->normals){n=pixel3d::normalize(n);if(pixel3d::length(n)<1e-12)n={0,0,1};}
  auto pixels=std::make_shared<pixel3d::ImageRGBA>(image.width,image.height);
  for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){const auto i=size_t(y*image.width+x)*image.component;pixels->at(x,y)={image.image[i],image.image[i+1],image.image[i+2],image.component==4?image.image[i+3]:uint8_t(255)};}
  result->texture=pixels;return result;
}

void write_geometry_binary(const pixel3d::Mesh& mesh, const std::filesystem::path& path) {
  validate(mesh); require(!std::filesystem::exists(path), "Refusing geometry overwrite");
  std::ofstream out(path, std::ios::binary);
  const uint32_t counts[] = {uint32_t(mesh.positions.size()), uint32_t(mesh.tris.size())};
  out.write(reinterpret_cast<const char*>(counts), 8);
  for (auto p : mesh.positions) {
    const float xyz[] = {float(p.x), float(p.y), float(p.z)};
    out.write(reinterpret_cast<const char*>(xyz), 12);
  }
  for (auto face : mesh.tris) out.write(reinterpret_cast<const char*>(face.data()), 12);
  require(bool(out), "Could not save geometry buffer");
}
pixel3d::Mesh read_geometry_binary(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary); uint32_t counts[2]{};
  in.read(reinterpret_cast<char*>(counts), 8);
  require(bool(in) && counts[0] && counts[1] && counts[0] <= kMaxVertices && counts[1] <= kMaxTriangles,
          "Invalid geometry buffer header");
  require(std::filesystem::file_size(path) == 8 + 12ull * (counts[0] + counts[1]), "Invalid geometry buffer size");
  pixel3d::Mesh mesh; mesh.positions.reserve(counts[0]); mesh.tris.resize(counts[1]);
  for (uint32_t i = 0; i < counts[0]; ++i) {
    float xyz[3]{}; in.read(reinterpret_cast<char*>(xyz), 12); mesh.positions.push_back({xyz[0], xyz[1], xyz[2]});
  }
  in.read(reinterpret_cast<char*>(mesh.tris.data()), std::streamsize(counts[1]) * 12);
  require(bool(in), "Truncated geometry buffer"); validate(mesh); return mesh;
}
nlohmann::json inspect_geometry(const pixel3d::Mesh& mesh) {
  validate(mesh);
  auto lo = mesh.positions.front(), hi = lo;
  for (auto p : mesh.positions) { lo = {std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
                                hi = {std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)}; }
  const auto diagonal = pixel3d::length(hi-lo);
  require(diagonal > 1e-12, "Negligible geometry extent");
  const auto t = pixel3d::analyze_topology(mesh, diagonal * diagonal * 1e-14);
  return {{"complete",true},{"vertices",mesh.positions.size()},{"triangles",mesh.tris.size()},
          {"boundary_edges",t.boundary_edges},{"overfull_edges",t.nonmanifold_edges},
          {"invalid_vertex_links",t.nonmanifold_vertices},{"inconsistent_winding_edges",t.inconsistent_winding_edges},
          {"duplicate_faces",t.duplicate_faces},{"internal_faces",t.internal_faces},{"zero_area_faces",t.zero_area_faces},
          {"components",t.shells},{"signed_volume",t.signed_volume},{"bbox_diagonal",diagonal},
          {"bounds",{{lo.x,lo.y,lo.z},{hi.x,hi.y,hi.z}}}};
}
Validation classify_validation(const nlohmann::json& t, const nlohmann::json& x) {
  try {
    if (!t.at("complete").get<bool>() || t.at("vertices").get<int64_t>() <= 0 || t.at("triangles").get<int64_t>() <= 0)
      return Validation::Unknown;
    for (auto key : {"overfull_edges","invalid_vertex_links","inconsistent_winding_edges","duplicate_faces","internal_faces","zero_area_faces","boundary_edges"}) {
      const auto& count=t.at(key);
      if(!count.is_number_integer() || count.get<int64_t>()<0)return Validation::Unknown;
      if(std::string_view(key)!="boundary_edges" && count.get<int64_t>()>0)return Validation::Unsafe;
    }
    if (t.at("vertices") != x.at("vertices") || t.at("triangles") != x.at("faces")) return Validation::Unknown;
    const auto& p = x.at("pair_classification"); const auto& s = x.at("edge_star_classification");
    if (!p.is_object() || !s.is_object()) return Validation::Unknown;
    const std::set<std::string> pair_keys{"empty","point_contact","edge_contact","edge_face_contact","transverse_crossing",
      "coplanar_area_overlap","near_contact_only","tolerance_sensitive","within_component_contacts","between_component_contacts"};
    const std::set<std::string> star_keys{"crossing_sheet_stars","noncrossing_edge_tangencies","angular_tie_or_degenerate","tolerance_sensitive"};
    for (const auto& [key, count] : p.items()) if (!pair_keys.contains(key) || !count.is_number_integer() || count.get<int64_t>() < 0) return Validation::Unknown;
    for (const auto& [key, count] : s.items()) if (!star_keys.contains(key) || !count.is_number_integer() || count.get<int64_t>() < 0) return Validation::Unknown;
    for (auto key : {"transverse_crossing","coplanar_area_overlap","edge_face_contact"})
      if (p.value(key,0) > 0) return Validation::Unsafe;
    if (s.value("crossing_sheet_stars",0) > 0) return Validation::Unsafe;
    if (!x.at("complete").get<bool>()) return Validation::Unknown;
    if (t.at("boundary_edges").get<int>() || p.value("tolerance_sensitive",0) ||
        p.value("point_contact",0) || p.value("edge_contact",0) || s.value("angular_tie_or_degenerate",0) || s.value("tolerance_sensitive",0))
      return Validation::Review;
    return Validation::Pass;
  } catch (...) { return Validation::Unknown; }
}
const char* validation_name(Validation s) {
  switch(s) { case Validation::Pass:return "PASS"; case Validation::Unsafe:return "UNSAFE";
              case Validation::Review:return "REVIEW"; default:return "UNKNOWN"; }
}
}  // namespace p3dasset
