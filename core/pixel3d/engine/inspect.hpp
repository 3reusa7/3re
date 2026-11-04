#pragma once
// Read-only JSON views of a session, shared by every client: the MCP server
// exposes them as resources, the GUI shows them in its panels. They never
// compute anything new — they only describe the document as it is.
#include <nlohmann/json.hpp>

#include "pixel3d/engine/session.hpp"

namespace pixel3d {

nlohmann::json inspect_project(const Session& s);      // settings, stage, history, checkpoints, status
nlohmann::json inspect_views(const Document& d);        // sources, alignment, per-view problems
nlohmann::json inspect_parts(const Document& d);        // bands, candidates, parts
nlohmann::json inspect_mesh(const Document& d);         // topology, cleanup, texture statistics
nlohmann::json inspect_conflicts(const Session& s);     // conflicts + ambiguous candidates
nlohmann::json inspect_diagnostics(const Session& s, size_t log_tail = 50);  // version, limits, log
nlohmann::json inspect_validation(const Document& d);   // last report or NOT_VALIDATED
nlohmann::json inspect_live(const Document& d);         // live checks after the last change
nlohmann::json inspect_edits(const Document& d);
nlohmann::json inspect_lowpoly(const Document& d);      // LOW-POLY STUDIO settings, typed parts, last build report        // edit list, user parts, masks summary
nlohmann::json parts_json(const Document& d);           // every part with names and flags

}  // namespace pixel3d
