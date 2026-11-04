#pragma once
// File access shared by every client. Reads are size-limited (untrusted input);
// writes are atomic (sibling temp file + rename) and fully checked, so a failed
// write is always reported and never leaves a truncated file at the target.
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"

namespace pixel3d {

// Whole file as bytes. Errors: io.not_found, io.read_failed, io.too_large.
Expected<std::vector<uint8_t>> read_file(const std::filesystem::path& path, uint64_t max_bytes);

// Creates missing parent directories. Returns a Message with code "ok"
// (Severity::Info) on success, otherwise io.write_failed with the path in `where`.
Message write_file_atomic(const std::filesystem::path& path, std::span<const uint8_t> bytes);
Message write_file_atomic(const std::filesystem::path& path, std::string_view text);

}  // namespace pixel3d
