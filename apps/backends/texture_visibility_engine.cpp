#include "texture_visibility_engine.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include <QSaveFile>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <queue>
#include <stdexcept>
#include <tuple>
namespace p3dasset {
using namespace pixel3d;
namespace {
void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
using Key=std::array<double,9>;
Key key(vec3d a,vec3d b,vec3d c){std::array<vec3d,3> p{a,b,c};Key best{};for(int r=0;r<3;++r){Key k{};for(int j=0;j<3;++j){auto v=p[size_t((j+r)%3)];k[size_t(3*j)]=v.x;k[size_t(3*j+1)]=v.y;k[size_t(3*j+2)]=v.z;}if(r==0||k<best)best=k;}return best;}
double edge(vec2d a,vec2d b,vec2d c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
template<class F> void raster(vec2d a,vec2d b,vec2d c,int w,int h,F f){
 double d=edge(a,b,c);if(std::abs(d)<1e-16)return;
 int x0=std::max(0,int(std::floor(std::min({a.x,b.x,c.x})))),x1=std::min(w-1,int(std::ceil(std::max({a.x,b.x,c.x}))));
 int y0=std::max(0,int(std::floor(std::min({a.y,b.y,c.y})))),y1=std::min(h-1,int(std::ceil(std::max({a.y,b.y,c.y}))));
 for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){vec2d p{double(x)+.5,double(y)+.5};double u=edge(b,c,p)/d,v=edge(c,a,p)/d,t=1-u-v;if(std::min({u,v,t})>=-1e-8)f(x,y,u,v,t);}
}
double linear(double x){x/=255;return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4);}
uint8_t srgb(double x){x=std::clamp(x,0.,1.);return uint8_t(std::lround(255*(x<=.0031308?12.92*x:1.055*std::pow(x,1/2.4)-.055)));}
vec3d color(QColor c){return {linear(c.red()),linear(c.green()),linear(c.blue())};}
Rgba8 encoded(vec3d c){return {srgb(c.x),srgb(c.y),srgb(c.z),255};}
}
VisibilityBake bake_visible_source(const Mesh& logical,const TexturedMesh& input,const QImage& original,int size,bool crisp){
 require(size==1024||size==2048,"Atlas must be 1024 or 2048");
 require(!logical.tris.empty()&&logical.tris.size()<=200000&&logical.positions.size()<=150000,"Bounded mesh profile exceeded");
 require(!original.isNull()&&original.width()<=4096&&original.height()<=4096,"Invalid or oversized source image");
 require(input.uvs.size()==input.positions.size()&&input.normals.size()==input.positions.size()&&input.indices.size()==logical.tris.size()*3,"Missing complete existing UV atlas");
 VisibilityBake result;auto& out=result.mesh;out=input;out.texture.reset();out.normal_map.reset();out.occlusion.reset();out.uvs1.clear();out.tangents.clear();
 Mesh rendered;rendered.positions=out.positions;for(size_t f=0;f<out.indices.size();f+=3)rendered.tris.push_back({out.indices[f],out.indices[f+1],out.indices[f+2]});
 require(geometrically_identical(logical,rendered),"Atlas differs from original geometry");
 std::map<Key,size_t> faces;for(size_t f=0;f<logical.tris.size();++f){auto t=logical.tris[f];require(faces.emplace(key(logical.positions[t[0]],logical.positions[t[1]],logical.positions[t[2]]),f).second,"Ambiguous coincident triangles; no guessed adjacency");}
 std::vector<size_t> lf(out.indices.size()/3);for(size_t f=0;f<lf.size();++f)lf[f]=faces.at(key(out.positions[out.indices[3*f]],out.positions[out.indices[3*f+1]],out.positions[out.indices[3*f+2]]));
 for(auto uv:out.uvs)require(std::isfinite(uv.x)&&std::isfinite(uv.y)&&uv.x>=0&&uv.y>=0&&uv.x<=1&&uv.y<=1,"Atlas UV out of bounds");
 // Reserve a narrow strip only when xatlas has retained point-collapsed faces.
 std::vector<size_t> collapsed;for(size_t f=0;f<lf.size();++f)if(std::abs(edge(out.uvs[out.indices[3*f]],out.uvs[out.indices[3*f+1]],out.uvs[out.indices[3*f+2]]))<1e-15)collapsed.push_back(f);
 require(collapsed.size()<=64,"Too many uncharted faces for conservative local UV recovery");
 int strip=collapsed.empty()?0:16;double shrink=double(size-strip)/size;
 if(strip){for(auto& uv:out.uvs)uv.y*=shrink;for(size_t n=0;n<collapsed.size();++n){size_t f=collapsed[n];double x=double(2+int(n)*12),y=double(size-14);require(x+8<size,"UV recovery strip full");std::array<vec2d,3> uv{{{x/size,y/size},{(x+6)/size,y/size},{x/size,(y+6)/size}}};for(int j=0;j<3;++j){auto old=out.indices[3*f+size_t(j)];out.indices[3*f+size_t(j)]=uint32_t(out.positions.size());out.positions.push_back(out.positions[old]);out.normals.push_back(out.normals[old]);out.uvs.push_back(uv[size_t(j)]);}}}
 int x0=original.width(),y0=original.height(),x1=-1,y1=-1;for(int y=0;y<original.height();++y)for(int x=0;x<original.width();++x)if(original.pixelColor(x,y).alpha()>=128){x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
 require(x1>=x0,"Source has no opaque subject");QImage source=original.copy(x0,y0,x1-x0+1,y1-y0+1).convertToFormat(QImage::Format_RGBA8888);
 auto lo=logical.positions.front(),hi=lo;for(auto p:logical.positions){lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};}
 require(hi.x>lo.x&&hi.y>lo.y,"Projection has zero width or height");double diagonal=length(hi-lo),eps=diagonal*1e-5;int w=source.width(),h=source.height();
 auto screen=[&](vec3d p){return vec2d{(p.x-lo.x)/(hi.x-lo.x)*w,(hi.y-p.y)/(hi.y-lo.y)*h};};
 std::vector<double> depth(size_t(w)*h,-std::numeric_limits<double>::infinity());std::vector<int> front(depth.size(),-1);
 for(size_t f=0;f<logical.tris.size();++f){auto t=logical.tris[f];auto a=logical.positions[t[0]],b=logical.positions[t[1]],c=logical.positions[t[2]];raster(screen(a),screen(b),screen(c),w,h,[&](int x,int y,double u,double v,double z){auto i=size_t(y*w+x);double d=a.z*u+b.z*v+c.z*z;if(d>depth[i]){depth[i]=d;front[i]=int(f);}});}
 auto visible=[&](vec3d p,vec3d n)->bool{if(n.z<.15)return false;auto q=screen(p);int x=std::clamp(int(q.x),0,w-1),y=std::clamp(int(q.y),0,h-1);int f=front[size_t(y*w+x)];if(f<0||source.pixelColor(x,y).alpha()<128)return false;auto t=logical.tris[size_t(f)];auto a=logical.positions[t[0]],b=logical.positions[t[1]],c=logical.positions[t[2]];auto sa=screen(a),sb=screen(b),sc=screen(c);double d=edge(sa,sb,sc);if(std::abs(d)<1e-16)return false;double u=edge(sb,sc,q)/d,v=edge(sc,sa,q)/d;return std::abs(p.z-(a.z*u+b.z*v+c.z*(1-u-v)))<=eps;};
 auto sample=[&](vec3d p){auto q=screen(p);if(crisp)return color(source.pixelColor(std::clamp(int(q.x),0,w-1),std::clamp(int(q.y),0,h-1)));double x=q.x-.5,y=q.y-.5;int ix=int(std::floor(x)),iy=int(std::floor(y));double fx=x-ix,fy=y-iy;vec3d c{};double sum=0;for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx){auto v=source.pixelColor(std::clamp(ix+dx,0,w-1),std::clamp(iy+dy,0,h-1));double wt=(dx?fx:1-fx)*(dy?fy:1-fy)*(v.alpha()/255.);if(v.alpha()<128)continue;c=c+color(v)*wt;sum+=wt;}return sum>0?c/sum:vec3d{};};
 // Dijkstra follows actual logical edges: no spatial nearest-neighbour jumps
 // between limbs or components and no coordinate welding of coincident shells.
 size_t nv=logical.positions.size();std::vector<std::vector<std::pair<uint32_t,double>>> adj(nv);std::vector<vec3d> colors(nv),normal(nv);std::vector<double> dist(nv,std::numeric_limits<double>::infinity());
 for(auto t:logical.tris){auto a=logical.positions[t[0]],b=logical.positions[t[1]],c=logical.positions[t[2]];auto n=cross(b-a,c-a);for(int j=0;j<3;++j){normal[t[size_t(j)]]=normal[t[size_t(j)]]+n;auto i=t[size_t(j)],k=t[size_t((j+1)%3)];double d=length(logical.positions[i]-logical.positions[k]);adj[i].push_back({k,d});adj[k].push_back({i,d});}}
 using Node=std::pair<double,uint32_t>;std::priority_queue<Node,std::vector<Node>,std::greater<Node>> queue;size_t seeds=0;
 for(size_t i=0;i<nv;++i)if(visible(logical.positions[i],normalize(normal[i]))){colors[i]=sample(logical.positions[i]);dist[i]=0;queue.push({0,uint32_t(i)});++seeds;}
 require(seeds>0,"No visible foreground correspondence; keep original material");
 while(!queue.empty()){auto [d,i]=queue.top();queue.pop();if(d!=dist[i])continue;for(auto [j,cost]:adj[i])if(d+cost<dist[j]){dist[j]=d+cost;colors[j]=colors[i];queue.push({dist[j],j});}}
 size_t unobserved=0;for(auto d:dist)unobserved+=!std::isfinite(d);
 // No invented colour for components with no observation. An explicit failure
 // keeps the original asset instead of silently transferring another component.
 require(unobserved==0,"A disconnected component has no visible color evidence; original preserved");
 // Joint bilateral diffusion uses the immutable nearest-observation color as
 // its range guide. It cannot average across large observed color differences.
 // These are color regions, not inferred semantic skin/metal/fabric labels.
 // Visible colors remain fixed. No component or spatial nearest-neighbor mixing.
 double hiddenResidual=0;size_t rejectedColorEdges=0;
 const auto guide=colors;
 std::vector<std::vector<std::pair<uint32_t,double>>> conductance(nv);
 for(size_t i=0;i<nv;++i)for(auto [j,d]:adj[i]){
   double delta=length(guide[i]-guide[j]);
   if(delta>.15){++rejectedColorEdges;continue;}
   double weight=std::exp(-delta*delta/(2*.05*.05))/std::max(d,diagonal*1e-8);
   conductance[i].push_back({j,weight});
 }
 std::vector<vec3d> next=colors;
 for(int iteration=0;iteration<64;++iteration){hiddenResidual=0;for(size_t i=0;i<nv;++i){if(dist[i]==0)continue;vec3d sum{};double weight=0;for(auto [j,wt]:conductance[i]){sum=sum+colors[j]*wt;weight+=wt;}if(weight>0){next[i]=colors[i]*.25+(sum/weight)*.75;hiddenResidual=std::max(hiddenResidual,length(next[i]-colors[i]));}}colors.swap(next);}
 auto texture=std::make_shared<ImageRGBA>(size,size);result.provenance=QImage(size,size,QImage::Format_RGBA8888);result.provenance.fill(Qt::transparent);
 std::vector<int> owner(size_t(size)*size,-1);std::vector<uint8_t> mode(owner.size(),0);size_t seen=0,inferred=0,overlap=0;
 for(size_t f=0;f<lf.size();++f){std::array<uint32_t,3> t{out.indices[3*f],out.indices[3*f+1],out.indices[3*f+2]};auto a=out.positions[t[0]],b=out.positions[t[1]],c=out.positions[t[2]];auto n=normalize(cross(b-a,c-a));auto l=logical.tris[lf[f]];std::array<vec3d,3> cs{};for(int j=0;j<3;++j){auto p=out.positions[t[size_t(j)]];bool found=false;for(auto k:l)if(logical.positions[k]==p){cs[size_t(j)]=colors[k];found=true;break;}require(found,"Atlas correspondence failed");}
 raster(out.uvs[t[0]]*size,out.uvs[t[1]]*size,out.uvs[t[2]]*size,size,size,[&](int x,int y,double u,double v,double z){auto idx=size_t(y*size+x);if(owner[idx]>=0&&owner[idx]!=int(f)){++overlap;return;}auto p=a*u+b*v+c*z;bool vis=visible(p,n);std::array<double,3> bary{u,v,z};size_t region=size_t(std::max_element(bary.begin(),bary.end())-bary.begin());vec3d fill{};double mass=0;for(size_t k=0;k<3;++k)if(length(cs[k]-cs[region])<=.15){fill=fill+cs[k]*bary[k];mass+=bary[k];}auto rgb=vis?sample(p):(mass>0?fill/mass:cs[region]);texture->at(x,y)=encoded(rgb);owner[idx]=int(f);mode[idx]=vis?1:2;seen+=vis;inferred+=!vis;result.provenance.setPixelColor(x,y,vis?QColor(0,200,80,255):QColor(255,160,0,255));});}
 require(seen>0,"No observed texels");
 // Nearest-owner gutter dilation never overwrites a covered texel. The current
 // nearest-filter export needs no mip chain; 4 texels protect base-level edges.
 std::vector<int> border;for(size_t i=0;i<mode.size();++i)if(mode[i])border.push_back(int(i));size_t begin=0,end=border.size();
 for(int round=0;round<4;++round){for(size_t k=begin;k<end;++k){int i=border[k],x=i%size,y=i/size;auto add=[&](int j){if(owner[size_t(j)]<0){owner[size_t(j)]=owner[size_t(i)];texture->at(j%size,j/size)=texture->at(x,y);border.push_back(j);}};if(x)add(i-1);if(y)add(i-size);if(x+1<size)add(i+1);if(y+1<size)add(i+size);}begin=end;end=border.size();}
 out.texture=texture;
 result.evidence={{"kind","source_visibility_completion"},{"neural_generated",false},{"physical_materials_estimated",false},{"camera","Approximate orthographic +Z, alpha-subject bbox to geometry XY bbox; not calibrated"},{"source_size",{w,h}},{"atlas_size",size},{"visible_seed_vertices",seeds},{"observed_texels",seen},{"inferred_texels",inferred},{"raster_overlap_samples",overlap},{"zero_uv_faces_before",collapsed.size()},{"zero_uv_faces_after",0},{"geometry_faces",logical.tris.size()},{"geometry_changed",false},{"hidden_method","Same-component geodesic observation + 64 joint bilateral iterations; hard color barriers; visible anchors fixed; no reflected artwork"},{"unobserved_vertices",unobserved},{"padding",4},{"hidden_bilateral_final_delta",hiddenResidual},{"color_barrier_directed_edges",rejectedColorEdges},{"semantic_material_labels",false},{"color_space","sRGB source decode -> linear interpolation -> sRGB Base Color; nearest for crisp"},{"metallic",0},{"roughness",1},{"pbr_note","Neutral display constants only, not inferred physical properties"},{"geometry_safety","UNCHANGED; texture completeness does not imply manifold or intersection safety"}};
 return result;
}
void save_visibility_bake(const VisibilityBake& bake,const Mesh& logical,const QString& path){
 require(!QFileInfo::exists(path),"Refusing existing output");ExportOptions options;options.center_bottom=false;options.units=ExportUnits::Pixels;options.generator="Pixel3D visibility-aware source approximation";auto bytes=write_glb(bake.mesh,options);
 // Preserve the separate logical topology across UV vertex splits. Buffers are
 // appended, never welded; original connected components remain inspectable.
 uint32_t n=0;std::memcpy(&n,bytes.data()+12,4);auto doc=nlohmann::json::parse(bytes.begin()+20,bytes.begin()+20+n);uint32_t bn=0;std::memcpy(&bn,bytes.data()+20+n,4);std::vector<uint8_t> bin(bytes.begin()+28+n,bytes.begin()+28+n+bn);
 auto add=[&](const void* data,size_t count,int component,const char* type,size_t number){while(bin.size()%4)bin.push_back(0);int view=int(doc["bufferViews"].size());doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",bin.size()},{"byteLength",count}});auto p=static_cast<const uint8_t*>(data);bin.insert(bin.end(),p,p+count);int acc=int(doc["accessors"].size());doc["accessors"].push_back({{"bufferView",view},{"componentType",component},{"type",type},{"count",number}});return acc;};
 std::vector<float> positions;for(auto p:logical.positions)positions.insert(positions.end(),{float(p.x),float(p.y),float(p.z)});std::vector<uint32_t> indices;for(auto t:logical.tris)indices.insert(indices.end(),t.begin(),t.end());int pa=add(positions.data(),positions.size()*4,5126,"VEC3",logical.positions.size()),ia=add(indices.data(),indices.size()*4,5125,"SCALAR",indices.size());
 doc["asset"]["extras"]["pixel3d_appearance"]={{"kind","source_visibility_completion"},{"hidden_surfaces","inferred, not ground truth"}};
 // Same topology schema consumed by the existing independent inspector.
 doc["asset"]["extras"]["pixel3d_topology"]={{"position_accessor",pa},{"index_accessor",ia}};
 doc["buffers"][0]["byteLength"]=bin.size();while(bin.size()%4)bin.push_back(0);std::string js=doc.dump();while(js.size()%4)js+=' ';std::vector<uint8_t> final;auto word=[&](uint32_t x){auto p=reinterpret_cast<uint8_t*>(&x);final.insert(final.end(),p,p+4);};word(0x46546c67);word(2);word(uint32_t(28+js.size()+bin.size()));word(uint32_t(js.size()));word(0x4e4f534a);final.insert(final.end(),js.begin(),js.end());word(uint32_t(bin.size()));word(0x004e4942);final.insert(final.end(),bin.begin(),bin.end());QSaveFile file(path);file.setDirectWriteFallback(false);require(file.open(QIODevice::WriteOnly),"Cannot create texture output");require(file.write(reinterpret_cast<const char*>(final.data()),qint64(final.size()))==qint64(final.size())&&file.commit(),"Texture output commit failed");
 require(geometrically_identical(logical,load_geometry_glb(std::filesystem::path(path.toStdWString()))),"Geometry export changed");
}
}


