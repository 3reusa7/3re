#pragma once
// JSON form of everything the user (or agent) decided — the inputs that fully
// determine a project. Derived data is never stored here; it is recomputed.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/document.hpp"

namespace pixel3d {

// Archive entry of a source image: views/<sha256>.<extension of its format>.
std::string source_image_entry(const SourceView& v);
// Whether `name` is a valid entry for the image `sha` (any supported extension).
bool is_source_image_entry(const std::string& name, const std::string& sha);

nlohmann::json polygons_to_json(const Polygons2D& p);
Polygons2D polygons_from_json(const nlohmann::json& j);

// Views are referenced by sha256 (image data is stored separately).
nlohmann::json inputs_to_json(const Document& d);

struct ImageBlob {
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  std::shared_ptr<const ImageRGBA> image;
};
// Rebuilds the input members of a Document (derived members stay empty).
Expected<Document> inputs_from_json(const nlohmann::json& j, const std::map<std::string, ImageBlob>& images);

// Hash of the inputs that determine the reconstructed mesh (cache key).
std::string mesh_input_hash(const Document& d);
// Key of the polished model: the mesh inputs, the texture inputs and the style.
std::string style_input_hash(const Document& d);

}  // namespace pixel3d
