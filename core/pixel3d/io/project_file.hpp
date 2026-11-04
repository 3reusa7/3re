#pragma once
// .p3d project file (spec §10): a ZIP holding
//   project.json        format, format_version, inputs, checkpoints, validation
//   views/<sha256>.png  every source image, byte-identical to the imported file
//   cache/meta.json + cache/mesh.bin   reconstructed mesh, valid only for its input hash
//   log.jsonl           the mutation log
// Saving is atomic (temp file + rename), so a crash never corrupts a project.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "pixel3d/engine/history.hpp"
#include "pixel3d/io/migrate.hpp"

namespace pixel3d {

struct LoadedProject {
  Document doc;  // inputs + mesh cache; derived data is recomputed on demand
  std::vector<LogEntry> log;
  std::map<std::string, Document> checkpoints;
};

Message save_project(const Document& doc, const std::vector<LogEntry>& log, const History& history,
                     const std::filesystem::path& path);
Expected<LoadedProject> load_project(const std::filesystem::path& path);

std::vector<uint8_t> encode_mesh_cache(const Mesh& m);
Expected<Mesh> decode_mesh_cache(std::span<const uint8_t> bytes);

}  // namespace pixel3d
