#pragma once
// Windows process runner and machine probe (PLAN.md, components 2 and 4).
// Every worker runs in its own job object: kill on close, optional memory limit, a time
// limit, and the job's own peak-memory accounting.
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "agent_spec.hpp"

namespace p3dagent {

struct ProcessSpec {
  fs::path exe;
  std::vector<std::string> args;  // UTF-8, quoted per the Windows argv rules by the runner
  fs::path cwd;
  double timeout_seconds = 1800;
  uint64_t memory_limit_mib = 0;  // 0 = none (job-wide committed memory)
  fs::path stdout_file, stderr_file;  // empty = discarded
  std::map<std::string, std::string> env;  // added to the inherited environment
};

struct ProcessResult {
  bool started = false, timed_out = false, memory_limit_hit = false;
  int exit_code = -1;
  double seconds = 0;
  uint64_t peak_memory_mib = 0;  // job peak committed memory
  // GPU memory in use on the NVIDIA device (whole device, desktop included; NVML, sampled every 200 ms while the
  // step runs). 0 = not measured (no NVIDIA driver / NVML).
  uint64_t vram_before_mib = 0, vram_peak_mib = 0;
  std::string error;
};

ProcessResult run_process(const ProcessSpec& spec);
// One argument quoted for CreateProcessW (backslashes before quotes doubled, quotes escaped).
std::wstring quote_windows_arg(const std::wstring& arg);
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

struct SystemInfo {
  uint64_t ram_total_mib = 0, ram_free_mib = 0, disk_free_mib = 0, vram_mib = 0;
  std::string gpu_name;
  bool gpu_present = false;
};
SystemInfo probe_system(const fs::path& disk_path);
// Device-wide GPU memory in use (MiB) on NVIDIA GPU 0 via NVML; 0 when unavailable.
uint64_t gpu_memory_used_mib();

// Streaming SHA-256 of a file (lower-case hex); empty when the file cannot be read.
std::string file_sha256(const fs::path& file);

}  // namespace p3dagent
