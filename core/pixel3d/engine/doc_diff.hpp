#pragma once
// What changed between two document snapshots, by id, for OpResult::changed_ids:
//   settings: [field...]          views: {added, removed, replaced} (roles)
//   decisions / depth_constraints / texture_assignments: {before, after} counts + ids
//   candidates: [ids whose status changed] (or "rebuilt" when the set changed)
//   parts: {added, removed, status_changed} (part ids of the new document)
//   mesh / texture / validation: {before, after} summaries
// Members that did not change are omitted, so {} means "nothing changed".
#include <nlohmann/json.hpp>

#include "pixel3d/engine/document.hpp"

namespace pixel3d {

nlohmann::json diff_documents(const Document& before, const Document& after);

}  // namespace pixel3d
