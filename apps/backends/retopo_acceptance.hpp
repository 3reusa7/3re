#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
namespace p3dasset::retopo {
inline void require_preserved_topology(const nlohmann::json& original, const nlohmann::json& result,
                                       int original_handles, int result_handles) {
  for(const auto* key : {"overfull_edges","invalid_vertex_links","inconsistent_winding_edges","duplicate_faces","zero_area_faces"})
    if(result.at(key).get<int64_t>() != 0) throw std::runtime_error("QuadWild result has invalid topology; original preserved");
  if(result.at("components") != original.at("components"))
    throw std::runtime_error("QuadWild changed component count; result rejected, original preserved");
  const auto before=original.at("boundary_edges").get<int64_t>(), after=result.at("boundary_edges").get<int64_t>();
  if(before>0 && after==0) throw std::runtime_error("QuadWild closed original boundaries; result rejected");
  if(before==0 && after>0) throw std::runtime_error("QuadWild introduced open boundaries; incomplete result rejected");
  if(before==0 && after==0 && original_handles!=result_handles)
    throw std::runtime_error("QuadWild changed tunnel topology; result rejected");
}
}
