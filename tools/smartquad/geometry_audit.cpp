// Read-only audit reused from the accepted TRELLIS geometry investigation.
// No geometry mutation or repair. Uses asset-relative tolerances, not voxel units.
#include "tri_bvh.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
using P=std::array<double,3>;
using T=std::array<P,3>;
P sub(P a,P b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
P add(P a,P b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
P mul(P a,double s){return {a[0]*s,a[1]*s,a[2]*s};}
double dot(P a,P b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
P cross(P a,P b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
double norm(P a){return std::sqrt(dot(a,a));}
P unit(P a){return mul(a,1/norm(a));}
P normal(const T&t){return unit(cross(sub(t[1],t[0]),sub(t[2],t[0])));}
enum Kind { Empty, PointContact, EdgeContact, EdgeFaceContact, Crossing, CoplanarOverlap };
const char* names[] = {"empty","point_contact","edge_contact","edge_face_contact","transverse_crossing","coplanar_area_overlap"};
struct Intersection {Kind kind=Empty;P point{};double span=0,area=0;};
bool interior(P p,const T&t,double eps){
    P n=normal(t);
    for(int k=0;k<3;++k){P e=sub(t[(k+1)%3],t[k]);if(dot(n,cross(e,sub(p,t[k])))<=eps*norm(e))return false;}
    return true;
}
std::vector<P> section(const T&t,const std::array<double,3>&d,double eps){
    std::vector<P> r;
    for(int k=0;k<3;++k){int j=(k+1)%3;
        if(std::abs(d[k])<=eps)r.push_back(t[k]);
        if((d[k]<-eps&&d[j]>eps)||(d[k]>eps&&d[j]<-eps))r.push_back(add(t[k],mul(sub(t[j],t[k]),d[k]/(d[k]-d[j]))));
    }
    return r;
}
Intersection intersect(const T&a,const T&b,double eps){
    P an=normal(a),bn=normal(b);std::array<double,3> da{},db{};
    for(int k=0;k<3;++k){da[k]=dot(bn,sub(a[k],b[0]));db[k]=dot(an,sub(b[k],a[0]));}
    auto separated=[&](const auto&d){return *std::min_element(d.begin(),d.end())>eps||*std::max_element(d.begin(),d.end())<-eps;};
    if(separated(da)||separated(db))return {};
    double maxdist=0,local=0;for(int k=0;k<3;++k){maxdist=std::max({maxdist,std::abs(da[k]),std::abs(db[k])});local=std::max({local,norm(sub(a[k],a[(k+1)%3])),norm(sub(b[k],b[(k+1)%3]))});}
    P direction=cross(an,bn);double angle=norm(direction);
    if(maxdist<=eps && angle<=eps/std::max(local,eps)){
        int drop=0;for(int k=1;k<3;++k)if(std::abs(an[k])>std::abs(an[drop]))drop=k;
        int u=(drop+1)%3,v=(drop+2)%3;
        auto orient=[&](P p,P q,P r){return (q[u]-p[u])*(r[v]-p[v])-(q[v]-p[v])*(r[u]-p[u]);};
        double sign=orient(b[0],b[1],b[2])>=0?1:-1;
        std::vector<P> polygon(a.begin(),a.end());
        for(int k=0;k<3&&!polygon.empty();++k){P p=b[k],q=b[(k+1)%3];double length=std::hypot(q[u]-p[u],q[v]-p[v]);std::vector<P> next;
            for(size_t j=0;j<polygon.size();++j){P s=polygon[j],t=polygon[(j+1)%polygon.size()];double ds=sign*orient(p,q,s),dt=sign*orient(p,q,t);bool ins=ds>=-eps*length,intt=dt>=-eps*length;
                if(ins)next.push_back(s);
                if(ins!=intt&&ds!=dt)next.push_back(add(s,mul(sub(t,s),(ds+eps*length)/(ds-dt))));
            }polygon.swap(next);
        }
        if(polygon.empty())return {};
        Intersection r;r.point={0,0,0};double projected=0,perimeter=0;
        for(size_t j=0;j<polygon.size();++j){P p=polygon[j],q=polygon[(j+1)%polygon.size()];r.point=add(r.point,p);projected+=p[u]*q[v]-p[v]*q[u];perimeter+=norm(sub(q,p));}
        r.point=mul(r.point,1.0/polygon.size());r.area=std::abs(projected)*.5/std::abs(an[drop]);
        for(P p:polygon)for(P q:polygon)r.span=std::max(r.span,norm(sub(p,q)));
        if(r.area>4*eps*perimeter){r.kind=CoplanarOverlap;return r;}
        r.kind=r.span>4*eps?EdgeContact:PointContact;return r;
    }
    if(angle<=1e-15)return {};
    direction=mul(direction,1/angle);
    auto sa=section(a,da,eps),sb=section(b,db,eps);if(sa.empty()||sb.empty())return {};
    auto range=[&](const auto&s){std::array<double,2>r{1e300,-1e300};for(P p:s){double t=dot(p,direction);r[0]=std::min(r[0],t);r[1]=std::max(r[1],t);}return r;};
    auto ra=range(sa),rb=range(sb);double lo=std::max(ra[0],rb[0]),hi=std::min(ra[1],rb[1]);if(hi<lo-eps)return {};
    Intersection r;r.span=std::max(0.0,hi-lo);r.point=add(sa[0],mul(direction,(lo+hi)*.5-dot(sa[0],direction)));
    bool ia=interior(r.point,a,eps),ib=interior(r.point,b,eps);
    r.kind=ia&&ib?Crossing:ia||ib?EdgeFaceContact:r.span>4*eps?EdgeContact:PointContact;
    return r;
}
struct DSU{std::vector<int>p;explicit DSU(int n):p(n,-1){}int find(int a){return p[a]<0?a:p[a]=find(p[a]);}void join(int a,int b){a=find(a);b=find(b);if(a==b)return;if(p[a]>p[b])std::swap(a,b);p[a]+=p[b];p[b]=a;}};
struct Mesh{std::vector<float>v;std::vector<int32_t>f;P point(int i)const{return {v[3*i],v[3*i+1],v[3*i+2]};}T tri(int i)const{return {point(f[3*i]),point(f[3*i+1]),point(f[3*i+2])};}};
struct Component{int vertices=0,faces=0;double area=0,volume=0;P lo{1e300,1e300,1e300},hi{-1e300,-1e300,-1e300};};
struct Example{int a,b,ca,cb;Intersection strict,scale;};
struct Audit{
    const Mesh&m;const std::vector<int>&fc;double strict_eps,scale_eps;int face=0;uint64_t candidates=0,expected=0;bool complete=true;std::map<std::string,uint64_t> counts;std::vector<Example> examples;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    bool common_feature(const Intersection&r,int i,int j){
        std::vector<P> common;for(int a=0;a<3;++a)for(int b=0;b<3;++b)if(m.f[3*i+a]==m.f[3*j+b])common.push_back(m.point(m.f[3*i+a]));
        // Two nondegenerate triangles with an indexed common edge have that
        // edge as their entire plane intersection unless they are coplanar.
        // Reconstructing its midpoint from nearly parallel planes can drift
        // into a face and falsely report an edge-face contact or crossing.
        // Topological adjacency establishes this case without that unstable
        // reconstruction. Coplanar area overlaps and duplicate faces remain
        // unsafe; a single common vertex never suppresses a true crossing.
        if(common.size()==2)return r.kind!=CoplanarOverlap;
        if(common.size()!=1||r.kind>EdgeContact)return false;
        return r.span<=4*strict_eps&&norm(sub(r.point,common[0]))<=4*strict_eps;
    }
    bool pair(int j){
        int i=face;if(j<=i)return true;++candidates;
        if(candidates>8000000 || ((candidates&4095)==0&&std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()>60)){complete=false;return false;}
        T a=m.tri(i),b=m.tri(j);auto s=intersect(a,b,strict_eps);
        if(s.kind!=Empty&&common_feature(s,i,j)){++expected;return true;}
        auto t=intersect(a,b,scale_eps);if(s.kind==Empty&&t.kind==Empty)return true;
        std::string label;
        if(s.kind==Empty)label="near_contact_only";
        else if(s.kind!=t.kind)label="tolerance_sensitive";
        else label=names[s.kind];
        ++counts[label];++counts[fc[i]==fc[j]?"within_component_contacts":"between_component_contacts"];
        if(examples.size()<512 && (s.kind>=EdgeFaceContact || label=="tolerance_sensitive"))examples.push_back({i,j,fc[i],fc[j],s,t});
        return true;
    }
};
void point_json(std::ostream&o,P p){o<<'['<<p[0]<<','<<p[1]<<','<<p[2]<<']';}
int star_class(const std::array<double,4>&angles,const std::array<int,4>&groups,double angular_eps){
    std::array<int,4>order{0,1,2,3};std::sort(order.begin(),order.end(),[&](int a,int b){return angles[a]<angles[b];});
    for(int k=0;k<4;++k){double d=angles[order[(k+1)%4]]-angles[order[k]];if(k==3)d+=2*std::acos(-1.0);if(d<=angular_eps)return 2;}
    bool alternate=true;for(int k=0;k<4;++k)alternate&=groups[order[k]]!=groups[order[(k+1)%4]];return alternate?1:0;
}
int main(int argc,char**argv){
    try{
        if(argc==2&&std::string(argv[1])=="--self-test"){
            T a{{{0,0,0},{1,0,0},{0,1,0}}};
            if(intersect(a,T{{{.2,.2,-1},{.2,.2,1},{.8,.2,0}}},1e-10).kind!=Crossing)throw std::runtime_error("crossing fixture");
            if(intersect(a,T{{{.1,.1,0},{.4,.1,0},{.1,.4,0}}},1e-10).kind!=CoplanarOverlap)throw std::runtime_error("coplanar fixture");
            if(intersect(a,T{{{0,0,0},{1,0,0},{0,-1,0}}},1e-10).kind!=EdgeContact)throw std::runtime_error("edge fixture");
            if(intersect(a,T{{{0,0,0},{-1,0,0},{0,-1,0}}},1e-10).kind!=PointContact)throw std::runtime_error("point fixture");
            if(intersect(a,T{{{0,0,1},{1,0,1},{0,1,1}}},1e-10).kind!=Empty)throw std::runtime_error("separation fixture");
            if(intersect(a,T{{{.2,.2,0},{.6,.2,0},{.4,.2,1}}},1e-10).kind!=EdgeFaceContact)throw std::runtime_error("edge-face fixture");
            for(double scale:{.001,1000.0}){T x=a,y{{{.2,.2,-1},{.2,.2,1},{.8,.2,0}}};for(P&p:x)p=mul(p,scale);for(P&p:y)p=mul(p,scale);if(intersect(x,y,1e-10*scale).kind!=Crossing)throw std::runtime_error("scale invariance fixture");}
            if(star_class({0,1.57079632679,3.14159265359,4.71238898038},{0,1,0,1},1e-8)!=1)throw std::runtime_error("star crossing");
            if(star_class({0,1.57079632679,3.14159265359,4.71238898038},{0,0,1,1},1e-8)!=0)throw std::runtime_error("star tangency");
            if(star_class({0,0,3.14159265359,4.71238898038},{0,1,0,1},1e-8)!=2)throw std::runtime_error("star tie");
            // Actual saved analytic ellipsoid faces 5/6. F32 coordinates are
            // preserved exactly; the original plane-section test classified
            // their indexed shared edge as an edge-face contact at strict eps.
            Mesh adjacent;
            adjacent.v={0.f,.5824922919273376f,-.7641208171844482f,
                        0.f,.6415247321128845f,-.590187668800354f,
                        .17382675409317017f,.6291979551315308f,-.590187668800354f,
                        .15783138573169708f,.5712997913360596f,-.7641208171844482f};
            adjacent.f={0,1,2,0,2,3};std::vector<int> adjacent_components{0,0};
            Audit adjacent_audit{adjacent,adjacent_components,3.58240015709398e-12,6.19888282926695e-7};
            auto adjacent_hit=intersect(adjacent.tri(0),adjacent.tri(1),adjacent_audit.strict_eps);
            if(adjacent_hit.kind==Empty||adjacent_hit.kind==CoplanarOverlap||!adjacent_audit.common_feature(adjacent_hit,0,1))throw std::runtime_error("saved nearly coplanar indexed edge fixture");
            adjacent_audit.face=0;adjacent_audit.pair(1);
            if(adjacent_audit.expected!=1||!adjacent_audit.counts.empty())throw std::runtime_error("saved indexed edge audit classification");
            Mesh overlapping;overlapping.v={0,0,0,1,0,0,0,1,0,.25f,.5f,0};overlapping.f={0,1,2,0,1,3};
            Audit overlap_audit{overlapping,adjacent_components,1e-12,1e-7};
            auto overlap_hit=intersect(overlapping.tri(0),overlapping.tri(1),1e-12);
            if(overlap_hit.kind!=CoplanarOverlap||overlap_audit.common_feature(overlap_hit,0,1))throw std::runtime_error("coplanar indexed-edge overlap remains unsafe");
            overlapping.f={0,1,2,0,1,2};
            if(overlap_audit.common_feature(intersect(overlapping.tri(0),overlapping.tri(1),1e-12),0,1))throw std::runtime_error("duplicate indexed face remains unsafe");
            // Independently confirmed saved banana faces 488/490 share one
            // indexed vertex but cross through both triangle interiors.
            Mesh saved_crossing;
            saved_crossing.v={-.4786738157272339f,-.004469250328838825f,.10059702396392822f,
                              -.4776306748390198f,-.004635159857571125f,.10183180868625641f,
                              -.4792191684246063f,.0012157682795077562f,.09966984391212463f,
                              -.4792797267436981f,-.004417741671204567f,.099198117852211f,
                              -.4774140417575836f,-.00014597491826862097f,.1037147268652916f};
            saved_crossing.f={0,1,2,3,1,4};
            Audit crossing_audit{saved_crossing,adjacent_components,1e-12,1e-7};
            auto saved_hit=intersect(saved_crossing.tri(0),saved_crossing.tri(1),1e-12);
            if(saved_hit.kind!=Crossing||crossing_audit.common_feature(saved_hit,0,1))throw std::runtime_error("saved banana common-vertex crossing remains unsafe");
            std::printf("PASS: saved ellipsoid indexed edge (%s), coplanar shared-edge overlap, duplicate face, saved banana common-vertex crossing\n",names[adjacent_hit.kind]);
            std::puts("PASS: crossing, coplanar overlap, shared edge, shared point, separation, alternating/tangent/tied edge stars");return 0;
        }
        if(argc!=3)throw std::runtime_error("usage: prequad_geometry_audit input.bin output.json");
        std::ifstream in(argv[1],std::ios::binary);uint32_t nv=0,nf=0;in.read((char*)&nv,4);in.read((char*)&nf,4);
        if(!in||nv>2000000||nf>4000000||!nv||!nf)throw std::runtime_error("bounded recovery profile exceeded (2M vertices / 4M faces)");
        Mesh m;m.v.resize(size_t(nv)*3);m.f.resize(size_t(nf)*3);in.read((char*)m.v.data(),12ull*nv);in.read((char*)m.f.data(),12ull*nf);if(!in)throw std::runtime_error("short input");
        for(float value:m.v)if(!std::isfinite(value))throw std::runtime_error("nonfinite coordinate");
        for(int32_t index:m.f)if(index<0||uint32_t(index)>=nv)throw std::runtime_error("index outside vertex range");
        for(uint32_t f=0;f<nf;++f){T t=m.tri(f);if(norm(cross(sub(t[1],t[0]),sub(t[2],t[0])))==0)throw std::runtime_error("degenerate triangle; input topology gate required");}
        P lo{1e300,1e300,1e300},hi{-1e300,-1e300,-1e300};double maxabs=0;
        for(uint32_t v=0;v<nv;++v)for(int k=0;k<3;++k){double p=m.v[3*v+k];if(!std::isfinite(p))throw std::runtime_error("nonfinite");lo[k]=std::min(lo[k],p);hi[k]=std::max(hi[k],p);maxabs=std::max(maxabs,std::abs(p));}
        DSU d(nv);for(uint32_t f=0;f<nf;++f){for(int k=0;k<3;++k)if(m.f[3*f+k]<0||m.f[3*f+k]>=int(nv))throw std::runtime_error("bad index");d.join(m.f[3*f],m.f[3*f+1]);d.join(m.f[3*f],m.f[3*f+2]);}
        std::map<int,int> component_ids;std::vector<int>vc(nv),fc(nf);for(uint32_t v=0;v<nv;++v){int r=d.find(v);if(!component_ids.count(r))component_ids[r]=int(component_ids.size());vc[v]=component_ids[r];}
        std::vector<Component>cs(component_ids.size());for(uint32_t v=0;v<nv;++v){auto&c=cs[vc[v]];++c.vertices;P p=m.point(v);for(int k=0;k<3;++k){c.lo[k]=std::min(c.lo[k],p[k]);c.hi[k]=std::max(c.hi[k],p[k]);}}
        for(uint32_t f=0;f<nf;++f){fc[f]=vc[m.f[3*f]];auto&c=cs[fc[f]];T t=m.tri(f);++c.faces;c.area+=norm(cross(sub(t[1],t[0]),sub(t[2],t[0])))*.5;c.volume+=dot(t[0],cross(t[1],t[2]))/6;}
        double diagonal=norm(sub(hi,lo)),voxel=0;double strict_eps=std::max(diagonal*1e-12,1e-30),scale_eps=std::max({4*FLT_EPSILON*maxabs,diagonal*1e-10,1e-30});
        auto bvh=trellis::TriBvh::build(m.v.data(),nv,m.f.data(),nf);Audit audit{m,fc,strict_eps,scale_eps};
        for(uint32_t f=0;f<nf&&audit.complete;++f){T t=m.tri(f);float mn[3],mx[3];for(int k=0;k<3;++k){mn[k]=float(std::min({t[0][k],t[1][k],t[2][k]})-scale_eps);mx[k]=float(std::max({t[0][k],t[1][k],t[2][k]})+scale_eps);}audit.face=f;bvh.visit_overlapping(mn,mx,[](void*p,int32_t j){return static_cast<Audit*>(p)->pair(j);},&audit);}
        // Coordinate groups and exact geometric edge stars; no welding.
        std::vector<int>order(nv),pos(nv);std::iota(order.begin(),order.end(),0);std::sort(order.begin(),order.end(),[&](int a,int b){return m.point(a)<m.point(b);});
        int groups=0;std::map<std::string,uint64_t> point_counts;for(size_t i=0;i<order.size();){size_t j=i+1;while(j<order.size()&&m.point(order[i])==m.point(order[j]))++j;bool between=false;for(size_t k=i;k<j;++k){pos[order[k]]=groups;between|=vc[order[k]]!=vc[order[i]];}if(j-i>1)++point_counts[between?"between_component_position_groups":"within_component_position_groups"];++groups;i=j;}
        struct Edge{int a,b,h;};std::vector<Edge>edges;edges.reserve(size_t(nf)*3);for(uint32_t h=0;h<m.f.size();++h){int a=pos[m.f[h]],b=pos[m.f[h-h%3+(h%3+1)%3]];edges.push_back({std::min(a,b),std::max(a,b),int(h)});}std::sort(edges.begin(),edges.end(),[](const Edge&a,const Edge&b){return std::array<int,3>{a.a,a.b,a.h}<std::array<int,3>{b.a,b.b,b.h};});
        struct Star{int a,b,copies,count,strict_class,scale_class;bool between;double altitude;};std::vector<Star>stars;std::map<std::string,uint64_t>star_counts;
        for(size_t begin=0;begin<edges.size();){size_t end=begin+1;while(end<edges.size()&&edges[end].a==edges[begin].a&&edges[end].b==edges[begin].b)++end;if(end-begin>2){
            std::map<std::pair<int,int>,int>ids;std::array<double,4>angles{};std::array<int,4>labels{};double altitude=1e300;bool between=false;int firstcomp=fc[edges[begin].h/3];
            int h0=edges[begin].h;P p=m.point(m.f[h0]),q=m.point(m.f[h0-h0%3+(h0%3+1)%3]);if(pos[m.f[h0]]!=edges[begin].a)std::swap(p,q);P axis=unit(sub(q,p));P basis=unit(cross(axis,std::abs(axis[0])<.8?P{1,0,0}:P{0,1,0}));P basis2=cross(axis,basis);
            size_t n=0;for(size_t k=begin;k<end;++k){int h=edges[k].h,a=m.f[h],b=m.f[h-h%3+(h%3+1)%3];auto key=std::minmax(a,b);if(!ids.count(key))ids[key]=int(ids.size());P ray=sub(m.point(m.f[h-h%3+(h%3+2)%3]),p);ray=sub(ray,mul(axis,dot(ray,axis)));altitude=std::min(altitude,norm(ray));if(n<4){double angle=std::atan2(dot(ray,basis2),dot(ray,basis));if(angle<0)angle+=2*std::acos(-1.0);angles[n]=angle;labels[n]=ids[key];}++n;between|=fc[h/3]!=firstcomp;}
            int s=2,t=2;if(n==4&&ids.size()==2&&altitude>scale_eps){s=star_class(angles,labels,strict_eps/altitude);t=star_class(angles,labels,scale_eps/altitude);}
            std::string label=s!=t?"tolerance_sensitive":s==1?"crossing_sheet_stars":s==0?"noncrossing_edge_tangencies":"angular_tie_or_degenerate";++star_counts[label];stars.push_back({edges[begin].a,edges[begin].b,int(ids.size()),int(n),s,t,between,altitude});
        }begin=end;}
        std::ifstream exists(argv[2]);if(exists.good())throw std::runtime_error("refusing existing report");std::ofstream out(argv[2]);out<<std::setprecision(17);
        out<<"{\n\"complete\":"<<(audit.complete?"true":"false")<<",\"vertices\":"<<nv<<",\"faces\":"<<nf<<",\"components\":"<<cs.size()<<",\"bbox_diagonal\":"<<diagonal<<",\"voxel_spacing\":"<<voxel<<",\"strict_tolerance\":"<<strict_eps<<",\"f32_scale_tolerance\":"<<scale_eps<<",\"candidate_pairs\":"<<audit.candidates<<",\"expected_topological_adjacencies\":"<<audit.expected<<",\"runtime_seconds\":"<<std::chrono::duration<double>(std::chrono::steady_clock::now()-audit.start).count()<<",\n";
        auto counts_json=[&](const char*name,const auto&counts){out<<'"'<<name<<"\":{";bool comma=false;for(auto&x:counts){if(comma)out<<',';comma=true;out<<'"'<<x.first<<"\":"<<x.second;}out<<"},\n";};counts_json("pair_classification",audit.counts);counts_json("edge_star_classification",star_counts);counts_json("point_position_groups",point_counts);
        out<<"\"component_records\":[";for(size_t i=0;i<cs.size();++i){if(i)out<<',';auto&c=cs[i];out<<"{\"id\":"<<i<<",\"vertices\":"<<c.vertices<<",\"faces\":"<<c.faces<<",\"area\":"<<c.area<<",\"signed_volume\":"<<c.volume<<",\"bbox_diagonal\":"<<norm(sub(c.hi,c.lo))<<",\"bounds\":[";point_json(out,c.lo);out<<',';point_json(out,c.hi);out<<"]}";}out<<"],\n\"edge_stars\":[";
        for(size_t i=0;i<stars.size();++i){if(i)out<<',';auto&s=stars[i];out<<"{\"position_ids\":["<<s.a<<','<<s.b<<"],\"copies\":"<<s.copies<<",\"incident_faces\":"<<s.count<<",\"strict_class\":"<<s.strict_class<<",\"scale_class\":"<<s.scale_class<<",\"between_components\":"<<(s.between?"true":"false")<<",\"minimum_altitude\":"<<s.altitude<<'}';}out<<"],\n\"examples\":[";
        for(size_t i=0;i<audit.examples.size();++i){if(i)out<<',';auto&e=audit.examples[i];out<<"{\"faces\":["<<e.a<<','<<e.b<<"],\"components\":["<<e.ca<<','<<e.cb<<"],\"strict\":\""<<names[e.strict.kind]<<"\",\"scale\":\""<<names[e.scale.kind]<<"\",\"span\":"<<e.strict.span<<",\"area\":"<<e.strict.area<<",\"point\":";point_json(out,e.strict.point);out<<'}';}out<<"],\n\"method\":\"Read-only F64 plane sections and coplanar polygon clipping at two scale-aware tolerances; all BVH candidates, 8M-pair/60-second cap. Exact edge-star alternation supplements face-pair contacts. Intent cannot be inferred from geometry alone.\"\n}\n";
        std::printf("complete=%d candidates=%llu components=%zu exact-edge-stars=%zu elapsed=%.3fs\n",audit.complete,(unsigned long long)audit.candidates,cs.size(),stars.size(),std::chrono::duration<double>(std::chrono::steady_clock::now()-audit.start).count());return audit.complete?0:2;
    }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}
