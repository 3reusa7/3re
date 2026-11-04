#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace p3dtrellis {
struct DegenerateReport { size_t input_faces=0, removed_faces=0, repeated_index_faces=0; };
// Only exact F64 cross-product zero on F32 points. No epsilon, position edits,
// welding, filling, positive-area face deletion or component merging. Callers
// must still run the existing topology and intersection audits afterwards.
inline DegenerateReport remove_exact_degenerate(std::vector<float>& verts,std::vector<int32_t>& faces) {
  if(verts.size()%3 || faces.size()%3)throw std::runtime_error("Invalid triangle storage");
  for(float v:verts)if(!std::isfinite(v))throw std::runtime_error("Nonfinite triangle coordinate");
  for(auto i:faces)if(i<0 || size_t(i)>=verts.size()/3)throw std::runtime_error("Invalid triangle index");
  DegenerateReport r;r.input_faces=faces.size()/3;
  size_t out=0;
  for(size_t f=0;f<faces.size();f+=3){
    const auto a=size_t(faces[f])*3,b=size_t(faces[f+1])*3,c=size_t(faces[f+2])*3;
    const double ux=double(verts[b])-verts[a],uy=double(verts[b+1])-verts[a+1],uz=double(verts[b+2])-verts[a+2];
    const double vx=double(verts[c])-verts[a],vy=double(verts[c+1])-verts[a+1],vz=double(verts[c+2])-verts[a+2];
    const double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
    if(nx==0 && ny==0 && nz==0){
      ++r.removed_faces;r.repeated_index_faces+=(a==b || b==c || c==a);continue;
    }
    for(int k=0;k<3;++k)faces[out++]=faces[f+size_t(k)];
  }
  faces.resize(out);return r;
}

struct ZeroLengthReport { size_t zero_length_edges=0, collapsed=0, faces_removed=0, skipped=0; };
// Dual contouring can emit two vertex ids at exactly the same F32 position,
// joined by a mesh edge. Both triangles on that edge then have zero area.
// Deleting them (remove_exact_degenerate) leaves a zero-width slit: four
// boundary edges that coincide pairwise. Collapsing the zero-length edge
// instead merges the two ids and closes the slit with no position change.
// Applied only when the edge has exactly two faces and the endpoints' common
// neighbours are exactly those faces' apexes (the edge-collapse link
// condition), so no overfull edge or duplicate face can result. Anything else
// is counted as skipped and left for remove_exact_degenerate.
inline ZeroLengthReport collapse_zero_length_edges(const std::vector<float>& verts,std::vector<int32_t>& faces) {
  if(verts.size()%3 || faces.size()%3)throw std::runtime_error("Invalid triangle storage");
  for(auto i:faces)if(i<0 || size_t(i)>=verts.size()/3)throw std::runtime_error("Invalid triangle index");
  const auto same=[&](int32_t a,int32_t b){
    return verts[3*size_t(a)]==verts[3*size_t(b)] && verts[3*size_t(a)+1]==verts[3*size_t(b)+1] && verts[3*size_t(a)+2]==verts[3*size_t(b)+2];
  };
  ZeroLengthReport r;
  std::vector<std::pair<int32_t,int32_t>> edges;
  for(size_t f=0;f<faces.size();f+=3)for(int k=0;k<3;++k){
    const int32_t a=faces[f+size_t(k)],b=faces[f+size_t((k+1)%3)];
    if(a!=b && same(a,b))edges.emplace_back(std::min(a,b),std::max(a,b));
  }
  std::sort(edges.begin(),edges.end());edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
  r.zero_length_edges=edges.size();
  if(edges.empty())return r;
  // Incident faces of the involved ids only; kept current while collapsing.
  std::unordered_map<int32_t,std::vector<size_t>> incident;
  for(const auto& [a,b]:edges){incident[a];incident[b];}
  for(size_t f=0;f<faces.size()/3;++f)for(int k=0;k<3;++k){
    const auto it=incident.find(faces[3*f+size_t(k)]);
    if(it!=incident.end())it->second.push_back(f);
  }
  std::vector<uint8_t> dead(faces.size()/3,0);
  std::unordered_map<int32_t,int32_t> alias;
  const auto resolve=[&](int32_t v){for(auto it=alias.find(v);it!=alias.end();it=alias.find(v))v=it->second;return v;};
  const auto has=[&](size_t f,int32_t v){return faces[3*f]==v || faces[3*f+1]==v || faces[3*f+2]==v;};
  for(auto [a,b]:edges){
    a=resolve(a);b=resolve(b);
    if(a==b){continue;}
    std::vector<size_t> on_edge,around;
    for(int32_t v:{a,b})for(size_t f:incident[v])if(!dead[f] && std::find(around.begin(),around.end(),f)==around.end())around.push_back(f);
    std::vector<int32_t> apex,left,right;
    for(size_t f:around){
      const bool ha=has(f,a),hb=has(f,b);
      for(int k=0;k<3;++k){const int32_t v=faces[3*f+size_t(k)];if(v==a || v==b)continue;
        if(ha && hb)apex.push_back(v);else if(ha)left.push_back(v);else right.push_back(v);}
      if(ha && hb)on_edge.push_back(f);
    }
    const auto unique=[](std::vector<int32_t>& x){std::sort(x.begin(),x.end());x.erase(std::unique(x.begin(),x.end()),x.end());};
    unique(apex);unique(left);unique(right);
    std::vector<int32_t> common;
    for(int32_t v:apex)common.push_back(v);
    for(int32_t v:left)if(std::binary_search(right.begin(),right.end(),v))common.push_back(v);
    unique(common);
    // A closed tetrahedron passes the neighbour test but would leave two
    // copies of one triangle: check the surviving faces as well.
    std::vector<std::array<int32_t,3>> surviving;
    for(size_t f:around){if(has(f,a) && has(f,b))continue;
      std::array<int32_t,3> t{faces[3*f],faces[3*f+1],faces[3*f+2]};for(auto& v:t)if(v==b)v=a;
      std::sort(t.begin(),t.end());surviving.push_back(t);}
    std::sort(surviving.begin(),surviving.end());
    const bool duplicate=std::adjacent_find(surviving.begin(),surviving.end())!=surviving.end();
    if(on_edge.size()!=2 || apex.size()!=2 || common!=apex || duplicate){++r.skipped;continue;}
    for(size_t f:on_edge){dead[f]=1;++r.faces_removed;}
    for(size_t f:incident[b]){if(dead[f])continue;for(int k=0;k<3;++k)if(faces[3*f+size_t(k)]==b)faces[3*f+size_t(k)]=a;incident[a].push_back(f);}
    incident[b].clear();alias[b]=a;++r.collapsed;
  }
  size_t out=0;
  for(size_t f=0;f<faces.size()/3;++f){if(dead[f])continue;for(int k=0;k<3;++k)faces[out++]=faces[3*f+size_t(k)];}
  faces.resize(out);return r;
}
}  // namespace p3dtrellis
