#include "agent_exec.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <fstream>
#include <vector>

namespace p3dagent {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring quote_windows_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

namespace {

struct Handle {
  HANDLE h = nullptr;
  ~Handle() {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};

HANDLE open_output(const fs::path& file) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  if (file.empty()) return CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  return CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The inherited environment plus additions, as a sorted-enough UNICODE block.
std::vector<wchar_t> environment_block(const std::map<std::string, std::string>& add) {
  std::vector<wchar_t> block;
  wchar_t* env = GetEnvironmentStringsW();
  for (const wchar_t* p = env; p && *p; p += wcslen(p) + 1) {
    const std::wstring entry(p);
    const auto eq = entry.find(L'=', 1);
    bool replaced = false;
    for (const auto& [k, v] : add)
      if (eq != std::wstring::npos && _wcsicmp(entry.substr(0, eq).c_str(), widen(k).c_str()) == 0) replaced = true;
    if (!replaced) block.insert(block.end(), entry.begin(), entry.end()), block.push_back(L'\0');
  }
  if (env) FreeEnvironmentStringsW(env);
  for (const auto& [k, v] : add) {
    const auto e = widen(k + "=" + v);
    block.insert(block.end(), e.begin(), e.end());
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

}  // namespace

ProcessResult run_process(const ProcessSpec& spec) {
  ProcessResult r;
  const auto t0 = std::chrono::steady_clock::now();
  std::error_code ec;
  if (!fs::is_regular_file(spec.exe, ec)) {
    r.error = "executable not found: " + narrow(spec.exe.wstring());
    return r;
  }
  std::wstring line = quote_windows_arg(spec.exe.wstring());
  for (const auto& a : spec.args) line += L" " + quote_windows_arg(widen(a));

  Handle job{CreateJobObjectW(nullptr, nullptr)};
  if (!job.h) {
    r.error = "CreateJobObject failed";
    return r;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (spec.memory_limit_mib) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
    limits.JobMemoryLimit = SIZE_T(spec.memory_limit_mib) << 20;
  }
  if (!SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
    r.error = "SetInformationJobObject failed";
    return r;
  }
  Handle out{open_output(spec.stdout_file)}, err{open_output(spec.stderr_file)};
  Handle in{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
  SetHandleInformation(in.h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = in.h;
  si.StartupInfo.hStdOutput = out.h;
  si.StartupInfo.hStdError = err.h;
  // Only the three standard handles are inherited.
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_buf(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
  HANDLE inherit[3] = {in.h, out.h, err.h};
  const bool attr_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                       UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
  if (attr_ok) si.lpAttributeList = attrs;
  auto env = environment_block(spec.env);
  PROCESS_INFORMATION pi{};
  std::wstring mutable_line = line;
  const std::wstring cwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();
  const BOOL ok = CreateProcessW(spec.exe.c_str(), mutable_line.data(), nullptr, nullptr, TRUE,
                                 CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                                     (attr_ok ? EXTENDED_STARTUPINFO_PRESENT : 0),
                                 env.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
  if (attr_ok) DeleteProcThreadAttributeList(attrs);
  if (!ok) {
    r.error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
    return r;
  }
  Handle process{pi.hProcess}, thread{pi.hThread};
  if (!AssignProcessToJobObject(job.h, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    r.error = "AssignProcessToJobObject failed";
    return r;
  }
  ResumeThread(pi.hThread);
  r.started = true;
  // Wait in 200 ms slices, sampling the GPU's memory in use (the step's own share is peak - before).
  r.vram_before_mib = r.vram_peak_mib = gpu_memory_used_mib();
  const auto deadline = spec.timeout_seconds > 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(spec.timeout_seconds * 1000))
                                                 : std::chrono::steady_clock::time_point::max();
  DWORD waited = WAIT_TIMEOUT;
  while (std::chrono::steady_clock::now() < deadline) {
    waited = WaitForSingleObject(pi.hProcess, 200);
    if (r.vram_before_mib) r.vram_peak_mib = std::max(r.vram_peak_mib, gpu_memory_used_mib());
    if (waited != WAIT_TIMEOUT) break;
  }
  if (waited == WAIT_TIMEOUT) waited = WaitForSingleObject(pi.hProcess, 0);  // exited right at the deadline: not a timeout
  if (waited == WAIT_TIMEOUT) {
    r.timed_out = true;
    TerminateJobObject(job.h, 124);
    WaitForSingleObject(pi.hProcess, 10000);
  }
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  r.exit_code = int(code);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  if (QueryInformationJobObject(job.h, JobObjectExtendedLimitInformation, &info, sizeof info, nullptr))
    r.peak_memory_mib = uint64_t(info.PeakJobMemoryUsed) >> 20;
  if (spec.memory_limit_mib) {
    JOBOBJECT_LIMIT_VIOLATION_INFORMATION violation{};
    const bool reported = QueryInformationJobObject(job.h, JobObjectLimitViolationInformation, &violation, sizeof violation, nullptr) &&
                          (violation.ViolationLimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY);
    // The job refuses commits above the limit; a failed run whose peak reached the limit hit it.
    r.memory_limit_hit = r.exit_code != 0 && (reported || (!r.timed_out && r.peak_memory_mib + 8 >= spec.memory_limit_mib));
  }
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

uint64_t gpu_memory_used_mib() {
  // NVML from the NVIDIA driver, loaded once; the same counter nvidia-smi reports.
  struct Nvml {
    using Init = int (*)();
    using Handle = int (*)(unsigned, void**);
    struct Memory { unsigned long long total, free, used; };
    using Info = int (*)(void*, Memory*);
    HMODULE lib = nullptr;
    void* device = nullptr;
    Info info = nullptr;
    Nvml() {
      // Only from System32 (where the NVIDIA driver installs it): never the application or current folder.
      wchar_t sys[MAX_PATH];
      const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
      if (n > 0 && n < MAX_PATH) lib = LoadLibraryExW((std::wstring(sys, n) + L"\\nvml.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!lib) return;
      auto init = reinterpret_cast<Init>(reinterpret_cast<void*>(GetProcAddress(lib, "nvmlInit_v2")));
      auto handle = reinterpret_cast<Handle>(reinterpret_cast<void*>(GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2")));
      info = reinterpret_cast<Info>(reinterpret_cast<void*>(GetProcAddress(lib, "nvmlDeviceGetMemoryInfo")));
      if (!init || !handle || !info || init() != 0 || handle(0, &device) != 0) {
        device = nullptr;
        info = nullptr;
      }
    }
  };
  static Nvml nvml;
  if (!nvml.info || !nvml.device) return 0;
  Nvml::Memory m{};
  return nvml.info(nvml.device, &m) == 0 ? uint64_t(m.used >> 20) : 0;
}

SystemInfo probe_system(const fs::path& disk_path) {
  SystemInfo s;
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  if (GlobalMemoryStatusEx(&m)) {
    s.ram_total_mib = m.ullTotalPhys >> 20;
    s.ram_free_mib = m.ullAvailPhys >> 20;
  }
  ULARGE_INTEGER free_bytes{};
  std::error_code ec;
  fs::path probe = disk_path;
  while (!probe.empty() && !fs::exists(probe, ec)) probe = probe.parent_path();
  if (GetDiskFreeSpaceExW(probe.empty() ? nullptr : probe.c_str(), &free_bytes, nullptr, nullptr)) s.disk_free_mib = free_bytes.QuadPart >> 20;
  // The adapter with the most dedicated memory (DXGI; no CUDA call).
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  if (dxgi) {
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    IDXGIFactory1* factory = nullptr;
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    if (create && SUCCEEDED(create(iid_factory1, reinterpret_cast<void**>(&factory))) && factory) {
      IDXGIAdapter1* adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(adapter->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
          const uint64_t mib = uint64_t(d.DedicatedVideoMemory) >> 20;
          // NVIDIA (vendor 0x10DE) is what TRELLIS's CUDA runtime needs.
          if (d.VendorId == 0x10DE && mib > s.vram_mib) {
            s.vram_mib = mib;
            s.gpu_name = narrow(d.Description);
            s.gpu_present = true;
          }
        }
        adapter->Release();
      }
      factory->Release();
    }
    FreeLibrary(dxgi);
  }
  return s;
}

std::string file_sha256(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {};
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  std::string hex;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
    std::vector<char> buf(4 << 20);
    bool good = true;
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      const auto n = in.gcount();
      if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(n), 0) != 0) good = false;
    }
    unsigned char digest[32];
    if (good && !in.bad() && BCryptFinishHash(hash, digest, 32, 0) == 0) {
      static const char* d = "0123456789abcdef";
      for (unsigned char c : digest) {
        hex.push_back(d[c >> 4]);
        hex.push_back(d[c & 15]);
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

}  // namespace p3dagent
