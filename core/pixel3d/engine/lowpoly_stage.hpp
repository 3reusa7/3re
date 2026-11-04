#pragma once
// LOW-POLY STUDIO inside a project: the Document's views, user parts and part
// types become the builder's input; the result (mesh, rig, texture, the GLB
// bytes and their independent round-trip check, the low-poly gate) is the
// derived Document::lowpoly. It carries the key of the inputs it was built
// from; a committed change of any input drops it (Session::apply).
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/document.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/gate.hpp"
#include "pixel3d/lowpoly/skinned_gltf.hpp"

namespace pixel3d {

struct LowpolyBuild {
  std::string key;  // lowpoly_input_hash of the document it was built from
  lowpoly::Input input;
  lowpoly::Result result;
  lowpoly::SkinnedExportOptions export_options;
  std::vector<uint8_t> glb;      // the file lowpoly_export writes, byte for byte
  lowpoly::SkinnedCheck check;   // tinygltf reload of `glb`
  lowpoly::LowpolyValidation validation;
  lowpoly::ExportDecision decision;
};

// Key of everything a low-poly build depends on (views, alignment and
// normalisation settings, part masks, names and types, low-poly settings, scale).
std::string lowpoly_input_hash(const Document& d);

// The builder's input from an aligned document (errors: lowpoly.no_parts, lowpoly.part_type_missing).
Expected<lowpoly::Input> lowpoly_input(const Document& d, std::vector<Message>* notes);

// Builds d.lowpoly (requires d.align). Reuses the current build when its key matches.
std::optional<Message> build_lowpoly_stage(Document& d, std::vector<Message>& notes);

// The current build when it matches the document, else nullptr.
const LowpolyBuild* current_lowpoly(const Document& d);

// Report for clients: status, triangles per part and tube, checks, conflicts, IoU, export decision.
nlohmann::json lowpoly_report(const LowpolyBuild& b);
// Short summary used in every OpResult of a Low-Poly Studio project.
nlohmann::json lowpoly_summary(const LowpolyBuild& b);

}  // namespace pixel3d
