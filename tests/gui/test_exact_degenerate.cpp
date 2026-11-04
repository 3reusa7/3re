#include "trellis/exact_degenerate.hpp"
#include <iostream>
#include <limits>
#define CHECK(x) do{if(!(x))return __LINE__;}while(false)
int main(){
  std::vector<float> p{0,0,0, 1,0,0, 0,1,0, 2,0,0, 0,1e-30f,0};
  const auto original=p;
  std::vector<int32_t> f{0,1,2, 0,1,3, 0,1,1, 0,1,4};
  auto r=p3dtrellis::remove_exact_degenerate(p,f);
  CHECK(r.input_faces==4 && r.removed_faces==2 && r.repeated_index_faces==1);
  CHECK(p==original);CHECK((f==std::vector<int32_t>{0,1,2,0,1,4}));
  CHECK(p3dtrellis::remove_exact_degenerate(p,f).removed_faces==0);
  f.push_back(99);const auto invalid=f;try{p3dtrellis::remove_exact_degenerate(p,f);return 10;}catch(const std::runtime_error&){}CHECK(f==invalid);
  f={0,1,99};const auto invalid_index=f;try{p3dtrellis::remove_exact_degenerate(p,f);return 11;}catch(const std::runtime_error&){}CHECK(f==invalid_index);
  p[0]=std::numeric_limits<float>::quiet_NaN();f={0,1,2};try{p3dtrellis::remove_exact_degenerate(p,f);return 12;}catch(const std::runtime_error&){}
  std::cout<<"PASS: exact collinear/repeated faces removed; positive-area thin triangle, positions, order and failure atomicity preserved\n";
}
