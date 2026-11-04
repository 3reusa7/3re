#include "texture_visibility_engine.hpp"
#include "backends/mesh_asset.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace p3dasset;using namespace pixel3d;
void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
TexturedMesh atlas(const Mesh& m){TexturedMesh a;for(size_t f=0;f<m.tris.size();++f){auto t=m.tris[f];auto n=normalize(cross(m.positions[t[1]]-m.positions[t[0]],m.positions[t[2]]-m.positions[t[0]]));double x=double(f%4)/4+.02,y=double(f/4)/4+.02;for(int j=0;j<3;++j){a.indices.push_back(uint32_t(a.positions.size()));a.positions.push_back(m.positions[t[size_t(j)]]);a.normals.push_back(n);a.uvs.push_back({x+(j==1?.20:0),y+(j==2?.20:0)});}}return a;}
int main(int argc,char**argv){QCoreApplication app(argc,argv);try{
 Mesh m;m.positions={{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}};m.tris={{4,5,6},{4,6,7},{0,2,1},{0,3,2},{0,4,7},{0,7,3},{1,2,6},{1,6,5},{3,7,6},{3,6,2},{0,1,5},{0,5,4}};
 QImage image(64,64,QImage::Format_RGBA8888);image.fill(Qt::red);for(int y=16;y<48;++y)for(int x=16;x<48;++x)image.setPixelColor(x,y,Qt::blue);for(int y=0;y<4;++y)for(int x=0;x<4;++x)image.setPixelColor(x,y,QColor(0,255,0,0));
 auto a=atlas(m);auto result=bake_visible_source(m,a,image,1024,true);check(result.evidence["observed_texels"].get<int>()>0&&result.evidence["inferred_texels"].get<int>()>0,"Visibility provenance missing");
 // Centre of visible front stays blue; back must not reflect the blue face.
 auto uv=a.uvs[0]*.4+a.uvs[1]*.1+a.uvs[2]*.5;auto front=result.mesh.texture->at(int(uv.x*1024),int(uv.y*1024));check(front.b>200&&front.r<5,"Front detail was lost");
 uv=a.uvs[6]*.2+a.uvs[7]*.3+a.uvs[8]*.5;auto back=result.mesh.texture->at(int(uv.x*1024),int(uv.y*1024));check(back.r>200&&back.b<5,"Hidden face received mirrored artwork");
 for(auto c:result.mesh.texture->pixels())check(c.g<5,"Transparent green background leaked");
 QTemporaryDir temp;save_visibility_bake(result,m,temp.filePath("visible.glb"));auto read=load_geometry_glb(std::filesystem::path(temp.filePath("visible.glb").toStdWString()));check(read.positions==m.positions&&read.tris==m.tris,"Logical topology changed across UV seams");check(*load_generated_appearance(std::filesystem::path(temp.filePath("visible.glb").toStdWString()))->texture==*result.mesh.texture,"Base color round-trip differs");
 a.uvs[0]=a.uvs[1]=a.uvs[2];auto repaired=bake_visible_source(m,a,image,1024,true);check(repaired.evidence["zero_uv_faces_before"]==1&&repaired.mesh.indices.size()==a.indices.size(),"UV-only recovery failed");
 auto dark=m;dark.positions.insert(dark.positions.end(),{{-.2,-.2,0},{.2,-.2,0},{0,.2,0}});dark.tris.push_back({8,9,10});bool rejected=false;try{bake_visible_source(dark,atlas(dark),image,1024,true);}catch(const std::exception&){rejected=true;}check(rejected,"Unobserved disconnected component silently colored");
 // Two visible color regions must not diffuse into purple hidden texels.
 QImage split(64,64,QImage::Format_RGBA8888);split.fill(Qt::red);for(int y=0;y<64;++y)for(int x=32;x<64;++x)split.setPixelColor(x,y,Qt::blue);
 auto guarded=bake_visible_source(m,atlas(m),split,1024,true);check(guarded.evidence["color_barrier_directed_edges"].get<int>()>0,"Color barrier not exercised");
 size_t hidden=0;for(int y=0;y<1024;++y)for(int x=0;x<1024;++x)if(guarded.provenance.pixelColor(x,y).red()==255){auto c=guarded.mesh.texture->at(x,y);++hidden;check((c.r>250&&c.b<5)||(c.b>250&&c.r<5),"Unobserved color regions blended across barrier");}check(hidden>0,"No hidden samples tested");
 // xatlas absolute area epsilon previously rejected this valid metric face.
 Mesh tiny;tiny.positions={{0,0,0},{.0001,0,0},{0,.0001,0}};tiny.tris={{0,1,2}};auto ta=create_source_atlas(tiny,1024);check(ta.indices.size()==3,"UV generation dropped a tiny face");auto u0=ta.uvs[ta.indices[0]],u1=ta.uvs[ta.indices[1]],u2=ta.uvs[ta.indices[2]];check(std::abs((u1.x-u0.x)*(u2.y-u0.y)-(u1.y-u0.y)*(u2.x-u0.x))>1e-12,"Positive-area face collapsed in UV");for(size_t k=0;k<3;++k)check(ta.positions[ta.indices[k]]==tiny.positions[k],"Atlas normalization changed exported geometry");
 QImage transparent(64,64,QImage::Format_RGBA8888);transparent.fill(Qt::transparent);rejected=false;try{bake_visible_source(m,atlas(m),transparent,1024,true);}catch(const std::exception&){rejected=true;}check(rejected,"Transparent source accepted");
 std::cout<<"PASS: visible detail, occluded back, alpha rejection, logical topology and texture round-trip, UV-only recovery, unobserved component rejection, empty alpha rejection, hidden color-region barrier\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}

