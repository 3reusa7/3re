#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "backends/mesh_asset.hpp"
#include "pixel3d/io/glb_writer.hpp"

using namespace p3dasset;
using nlohmann::json;
namespace {
pixel3d::Mesh square() {
  pixel3d::Mesh mesh;
  mesh.positions = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}};
  mesh.tris = {{0,1,2}, {0,2,3}};
  return mesh;
}
pixel3d::TexturedMesh seam_square() {
  const auto source = square();
  pixel3d::TexturedMesh mesh;
  for (auto face : source.tris) for (auto i : face) {
    mesh.indices.push_back(uint32_t(mesh.positions.size()));
    mesh.positions.push_back(source.positions[i]);
  }
  // Deliberately authored, valid normals differ from geometric face normals.
  mesh.normals = {{1,0,0}, {1,0,0}, {1,0,0}, {0,1,0}, {0,1,0}, {0,1,0}};
  mesh.uvs = {{0,0}, {1,0}, {1,1}, {.2,.2}, {.8,.8}, {0,1}};
  auto image = std::make_shared<pixel3d::ImageRGBA>(2,2);
  image->at(0,0) = {255,0,0,255}; image->at(1,0) = {0,255,0,255};
  image->at(0,1) = {0,0,255,255}; image->at(1,1) = {255,255,0,255};
  mesh.texture = image;
  return mesh;
}
template<class T> void append(std::vector<uint8_t>& bytes, T value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(),p,p+sizeof(T));
}
uint32_t u32(const std::vector<uint8_t>& bytes, size_t offset) {
  uint32_t v; std::memcpy(&v,bytes.data()+offset,4); return v;
}
struct Fixture { json model; std::vector<uint8_t> binary; };
Fixture fixture(bool original_topology = true) {
  pixel3d::ExportOptions opt; opt.units = pixel3d::ExportUnits::Pixels; opt.center_bottom = false;
  const auto glb = pixel3d::write_glb(seam_square(),opt);
  const auto json_length = u32(glb,12), bin_length = u32(glb,20+json_length);
  Fixture f{json::parse(glb.begin()+20,glb.begin()+20+json_length),
            std::vector<uint8_t>(glb.begin()+28+json_length,glb.begin()+28+json_length+bin_length)};
  f.model["asset"]["extras"]["pixel3d_appearance"] = {{"kind","generated"}};
  if (original_topology) {
    const auto source = square();
    const auto positions_start = f.binary.size();
    for (auto p : source.positions) for (double c : {p.x,p.y,p.z}) append(f.binary,float(c));
    const auto pv = f.model["bufferViews"].size();
    f.model["bufferViews"].push_back({{"buffer",0},{"byteOffset",positions_start},{"byteLength",source.positions.size()*12},{"target",34962}});
    const auto pa = f.model["accessors"].size();
    f.model["accessors"].push_back({{"bufferView",pv},{"componentType",5126},{"count",source.positions.size()},{"type","VEC3"},{"min",{0,0,0}},{"max",{1,1,0}}});
    const auto indices_start = f.binary.size();
    for (auto face : source.tris) for (auto i : face) append(f.binary,i);
    const auto iv = f.model["bufferViews"].size();
    f.model["bufferViews"].push_back({{"buffer",0},{"byteOffset",indices_start},{"byteLength",source.tris.size()*12},{"target",34963}});
    const auto ia = f.model["accessors"].size();
    f.model["accessors"].push_back({{"bufferView",iv},{"componentType",5125},{"count",source.tris.size()*3},{"type","SCALAR"}});
    f.model["asset"]["extras"]["pixel3d_topology"] = {{"position_accessor",pa},{"index_accessor",ia}};
    f.model["buffers"][0]["byteLength"] = f.binary.size();
  }
  return f;
}
std::filesystem::path save(const QTemporaryDir& dir, const Fixture& f, const char* name = "fixture.glb") {
  auto text = f.model.dump(); while(text.size()%4)text.push_back(' ');
  std::vector<uint8_t> bytes;
  for (auto v : {uint32_t(0x46546c67),uint32_t(2),uint32_t(28+text.size()+f.binary.size()),uint32_t(text.size()),uint32_t(0x4e4f534a)}) append(bytes,v);
  bytes.insert(bytes.end(),text.begin(),text.end());
  append(bytes,uint32_t(f.binary.size())); append(bytes,uint32_t(0x004e4942));
  bytes.insert(bytes.end(),f.binary.begin(),f.binary.end());
  const auto path = std::filesystem::path(dir.filePath(name).toStdWString());
  std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  if(!out)throw std::runtime_error("Fixture write failed"); return path;
}
void metallic_roughness(Fixture& f) {
  f.model["samplers"].push_back({{"minFilter",9729},{"magFilter",9729},{"wrapS",33071},{"wrapT",33071}});
  f.model["textures"].push_back({{"source",0},{"sampler",1}});
  auto& pbr = f.model["materials"][0]["pbrMetallicRoughness"];
  pbr["metallicRoughnessTexture"] = {{"index",1}}; pbr["metallicFactor"] = .7; pbr["roughnessFactor"] = .4;
}
}  // namespace

class GeneratedMaterialTests : public QObject {
  Q_OBJECT
 private slots:
  void orientedGeometryComparisonPreservesMultiplicity() {
    const auto original = square();
    auto reordered = original;
    reordered.tris = {{2,3,0},{1,2,0}};
    QVERIFY(geometrically_identical(original,reordered));
    std::swap(reordered.tris[0][0],reordered.tris[0][1]);
    QVERIFY(!geometrically_identical(original,reordered));
    reordered = original; reordered.tris[1] = reordered.tris[0];
    QVERIFY(!geometrically_identical(original,reordered));
    reordered = original; reordered.positions[0].x = -0.0;
    QVERIFY(geometrically_identical(original,reordered));
    reordered.positions[1].x += 1e-9; // Same stored F32 coordinate.
    QVERIFY(geometrically_identical(original,reordered));
    reordered.positions[1].x += 1e-3;
    QVERIFY(!geometrically_identical(original,reordered));
  }
  void seamVerticesRemainSeparateFromLogicalGeometry() {
    QTemporaryDir dir; const auto path = save(dir,fixture());
    const auto logical = load_geometry_glb(path), original = square();
    QCOMPARE(logical.positions.size(),size_t(4)); QVERIFY(logical.tris == original.tris);
    const auto appearance = load_generated_appearance(path); QVERIFY(appearance);
    QCOMPARE(appearance->positions.size(),size_t(6));
    const auto expected_uvs = seam_square().uvs;
    for (size_t i=0;i<expected_uvs.size();++i) {
      QCOMPARE(appearance->uvs[i].x,double(float(expected_uvs[i].x)));
      QCOMPARE(appearance->uvs[i].y,double(float(expected_uvs[i].y)));
    }
    QVERIFY(appearance->normals == seam_square().normals);
    QCOMPARE(appearance->texture->at(0,0).r,uint8_t(255));
    QCOMPARE(appearance->texture->at(0,1).b,uint8_t(255));
    auto render = original; render.positions = appearance->positions; render.tris = {{0,1,2},{3,4,5}};
    QVERIFY(geometrically_identical(original,render));
    const auto absent = load_geometry_glb(save(dir,fixture(false),"without-meta.glb"));
    QCOMPARE(absent.positions.size(),size_t(6));
  }
  void forgedOriginalTopologyIsRejected() {
    QTemporaryDir dir; auto f = fixture();
    const auto pa = f.model["asset"]["extras"]["pixel3d_topology"]["position_accessor"].get<size_t>();
    const auto view = f.model["accessors"][pa]["bufferView"].get<size_t>();
    const auto offset = f.model["bufferViews"][view]["byteOffset"].get<size_t>();
    const float moved = .25f; std::memcpy(f.binary.data()+offset,&moved,4);
    const auto path = save(dir,f); QVERIFY_EXCEPTION_THROWN(load_geometry_glb(path),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(load_generated_appearance(path),std::runtime_error);
    f = fixture();
    const auto ia = f.model["asset"]["extras"]["pixel3d_topology"]["index_accessor"].get<size_t>();
    const auto iv = f.model["accessors"][ia]["bufferView"].get<size_t>();
    const auto io = f.model["bufferViews"][iv]["byteOffset"].get<size_t>();
    const uint32_t reversed[] = {0,2,1}; std::memcpy(f.binary.data()+io,reversed,12);
    QVERIFY_EXCEPTION_THROWN(load_geometry_glb(save(dir,f,"reversed.glb")),std::runtime_error);
  }
  void metallicRoughnessAndNormalsAreRetained() {
    QTemporaryDir dir; auto f = fixture(); metallic_roughness(f);
    f.model["materials"][0]["extensions"]["KHR_materials_unlit"] = json::object();
    f.model["extensionsUsed"] = {"KHR_materials_unlit"};
    const auto path = save(dir,f); const auto info = material_info(path);
    QCOMPARE(info["kind"].get<std::string>(),std::string("generated"));
    QCOMPARE(info["metallic_factor"].get<double>(),.7); QCOMPARE(info["roughness_factor"].get<double>(),.4);
    QVERIFY(info["unlit"].get<bool>()); QVERIFY(info["maps"].contains("metallic_roughness"));
    QVERIFY(info["maps"]["metallic_roughness"]["preview_filter_approximate"].get<bool>());
    QVERIFY(!info["preview_filter_approximate"].get<bool>());
    const auto appearance = load_generated_appearance(path);
    QVERIFY(appearance->normals == seam_square().normals);
    QCOMPARE(load_geometry_glb(path).positions.size(),size_t(4));
  }
  void interleavedOffsetsAndInvalidLayouts() {
    QTemporaryDir dir;auto f=fixture();auto& attrs=f.model["meshes"][0]["primitives"][0]["attributes"];
    const auto positions=seam_square();const auto start=f.binary.size();
    for(size_t i=0;i<positions.positions.size();++i){
      append(f.binary,uint32_t(0x12345678));
      for(double c:{positions.positions[i].x,positions.positions[i].y,positions.positions[i].z})append(f.binary,float(c));
      for(double c:{positions.normals[i].x,positions.normals[i].y,positions.normals[i].z})append(f.binary,float(c));
      append(f.binary,float(positions.uvs[i].x));append(f.binary,float(positions.uvs[i].y));
    }
    const auto view=f.model["bufferViews"].size();
    f.model["bufferViews"].push_back({{"buffer",0},{"byteOffset",start},{"byteLength",positions.positions.size()*36},{"byteStride",36},{"target",34962}});
    for(const auto& [name,offset]:std::vector<std::pair<std::string,size_t>>{{"POSITION",4},{"NORMAL",16},{"TEXCOORD_0",28}}){
      auto& a=f.model["accessors"][attrs[name].get<size_t>()];a["bufferView"]=view;a["byteOffset"]=offset;
    }
    f.model["buffers"][0]["byteLength"]=f.binary.size();
    auto appearance=load_generated_appearance(save(dir,f,"interleaved.glb"));
    QVERIFY(appearance->positions==positions.positions);QVERIFY(appearance->normals==positions.normals);
    for(size_t i=0;i<positions.uvs.size();++i){QCOMPARE(appearance->uvs[i].x,double(float(positions.uvs[i].x)));QCOMPARE(appearance->uvs[i].y,double(float(positions.uvs[i].y)));}
    auto bad=f;bad.model["bufferViews"][view]["byteStride"]=8;
    QVERIFY_EXCEPTION_THROWN(load_generated_appearance(save(dir,bad,"short-stride.glb")),std::runtime_error);
    bad=f;bad.model["accessors"][attrs["POSITION"].get<size_t>()]["byteOffset"]=2;
    QVERIFY_EXCEPTION_THROWN(load_geometry_glb(save(dir,bad,"unaligned.glb")),std::runtime_error);
    bad=f;bad.model["bufferViews"][view]["byteLength"]=positions.positions.size()*36-1;
    QVERIFY_EXCEPTION_THROWN(load_generated_appearance(save(dir,bad,"short-uv.glb")),std::runtime_error);
    bad=f;const auto ia=f.model["meshes"][0]["primitives"][0]["indices"].get<size_t>();
    bad.model["bufferViews"][f.model["accessors"][ia]["bufferView"].get<size_t>()]["byteStride"]=4;
    QVERIFY_EXCEPTION_THROWN(load_geometry_glb(save(dir,bad,"strided-indices.glb")),std::runtime_error);
  }
  void rawRecoveryTriangleBudget() {
    QTemporaryDir dir;auto f=fixture(false);const size_t count=2000001;
    const auto start=f.binary.size();f.binary.reserve(start+count*12);
    for(size_t i=0;i<count;++i)for(uint32_t j:{0u,1u,2u})append(f.binary,j);
    const auto view=f.model["bufferViews"].size();
    f.model["bufferViews"].push_back({{"buffer",0},{"byteOffset",start},{"byteLength",count*12},{"target",34963}});
    const auto accessor=f.model["accessors"].size();
    f.model["accessors"].push_back({{"bufferView",view},{"componentType",5125},{"count",count*3},{"type","SCALAR"}});
    f.model["meshes"][0]["primitives"][0]["indices"]=accessor;f.model["buffers"][0]["byteLength"]=f.binary.size();
    const auto mesh=load_geometry_glb(save(dir,f,"raw-over-two-million.glb"));QCOMPARE(mesh.tris.size(),count);
    QCOMPARE(mesh.tris.front(),mesh.tris.back());
    f.model["accessors"][accessor]["count"]=size_t(4000001)*3;
    QVERIFY_EXCEPTION_THROWN(load_geometry_glb(save(dir,f,"over-budget.glb")),std::runtime_error);
  }
  void unsupportedAppearanceNeverSilentlyChanges() {
    QTemporaryDir dir; auto f = fixture();
    f.model["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {.5,1,1,1};
    QVERIFY_EXCEPTION_THROWN(load_generated_appearance(save(dir,f,"factor.glb")),std::runtime_error);
    f = fixture(); f.model["materials"][0]["alphaMode"] = "BLEND";
    QVERIFY_EXCEPTION_THROWN(material_info(save(dir,f,"alpha.glb")),std::runtime_error);
    f = fixture(); f.model["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 1;
    QVERIFY_EXCEPTION_THROWN(material_info(save(dir,f,"uv1.glb")),std::runtime_error);
    f = fixture(); f.model["samplers"][0]["minFilter"] = 9987;
    QVERIFY_EXCEPTION_THROWN(load_generated_appearance(save(dir,f,"mipmap.glb")),std::runtime_error);
    f = fixture(); f.model["samplers"].erase(0); f.model["textures"][0].erase("sampler");
    const auto info = material_info(save(dir,f,"default.glb")); QVERIFY(info["preview_filter_approximate"].get<bool>());
  }
};
QTEST_GUILESS_MAIN(GeneratedMaterialTests)
#include "test_generated_material.moc"
