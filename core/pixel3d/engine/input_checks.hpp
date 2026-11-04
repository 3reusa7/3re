#pragma once
// Range checks for every value a client or a project file can supply. The same
// checks guard the Session API (CLI, MCP, GUI) and .p3d loading, so a value that
// cannot be set through the API cannot be smuggled in through a file either.
// Each returns std::nullopt when valid, else an Error message (code input.invalid).
#include <optional>

#include "pixel3d/engine/document.hpp"

namespace pixel3d {

std::optional<Message> check_settings(const ProjectSettings& s);
std::optional<Message> check_cleanup_options(const CleanupOptions& o);
std::optional<Message> check_texture_options(const TextureOptions& o);
std::optional<Message> check_depth_constraint(const DepthConstraint& c);
std::optional<Message> check_texture_assignment(const TextureAssignment& a);
std::optional<Message> check_decision(const UserDecision& u);
// All input members of a document (used after loading a project file).
std::optional<Message> check_document_inputs(const Document& d);

}  // namespace pixel3d
