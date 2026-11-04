#pragma once
// Crash reports (spec M5 §2): an unhandled structured exception or an uncaught
// C++ exception writes crash-<UTC time>.txt (reason, exception code, address,
// module + offset, version, thread) to %LOCALAPPDATA%\Pixel3D\crashes. The
// report is written from fixed buffers, without the heap, so it still works
// when the crash came from heap corruption. Windows only; elsewhere a no-op.
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace pixel3d {

// Installs the handlers once per process; `app` names the program in the report.
// `dir` overrides the folder (tests); empty = %LOCALAPPDATA%\Pixel3D\crashes.
void install_crash_handler(std::string_view app, const std::filesystem::path& dir = {});

// The folder reports go to (the default when no handler was installed).
std::filesystem::path crash_dir();

// Writes a report now; the handlers call this. False when nothing could be written.
bool write_crash_report(const char* reason, uint32_t code, const void* address) noexcept;

// Reports in crash_dir(), oldest first (the names sort by time).
std::vector<std::filesystem::path> list_crash_reports();

}  // namespace pixel3d
