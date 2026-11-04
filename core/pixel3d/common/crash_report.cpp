#include "pixel3d/common/crash_report.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "pixel3d/common/version.hpp"

namespace pixel3d {
namespace fs = std::filesystem;

#ifdef _WIN32
namespace {

// Everything the handler needs is prepared at install time, in fixed buffers.
wchar_t g_dir[MAX_PATH + 1] = {};
char g_app[64] = "3re";
bool g_installed = false;

fs::path default_dir() {
  wchar_t buf[MAX_PATH + 1];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  fs::path base = n > 0 && n < MAX_PATH ? fs::path(buf) : fs::temp_directory_path();
  return base / L"3re" / L"crashes";
}

const char* code_name(uint32_t code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case 0xC0000409u: return "stack buffer overrun / fail-fast";
    case 0: return "uncaught C++ exception";
    default: return "unhandled exception";
  }
}

LONG WINAPI on_seh(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
  write_crash_report("unhandled structured exception", rec ? static_cast<uint32_t>(rec->ExceptionCode) : 0u,
                     rec ? rec->ExceptionAddress : nullptr);
  return EXCEPTION_CONTINUE_SEARCH;  // Windows Error Reporting still runs as usual
}

[[noreturn]] void on_terminate() {
  char reason[512] = "uncaught C++ exception";
  if (const std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& ex) {
      std::snprintf(reason, sizeof reason, "uncaught C++ exception: %s", ex.what());
    } catch (...) {
      std::snprintf(reason, sizeof reason, "uncaught C++ exception of unknown type");
    }
  } else {
    std::snprintf(reason, sizeof reason, "std::terminate called without an active exception");
  }
  write_crash_report(reason, 0, nullptr);
  std::abort();
}

}  // namespace

void install_crash_handler(std::string_view app, const fs::path& dir) {
  const fs::path d = dir.empty() ? default_dir() : dir;
  const std::wstring w = d.wstring();
  const size_t n = std::min(w.size(), static_cast<size_t>(MAX_PATH));
  std::copy_n(w.c_str(), n, g_dir);
  g_dir[n] = 0;
  const size_t a = std::min(app.size(), sizeof g_app - 1);
  std::copy_n(app.data(), a, g_app);
  g_app[a] = 0;
  if (!g_installed) {
    SetUnhandledExceptionFilter(on_seh);
    std::set_terminate(on_terminate);
    g_installed = true;
  }
}

fs::path crash_dir() { return g_dir[0] ? fs::path(g_dir) : default_dir(); }

bool write_crash_report(const char* reason, uint32_t code, const void* address) noexcept {
  wchar_t dir[MAX_PATH + 1];
  if (g_dir[0]) {
    std::copy_n(g_dir, MAX_PATH + 1, dir);
  } else {
    // No handler installed (tests, or a direct call): resolve the default now.
    try {
      const std::wstring w = default_dir().wstring();
      const size_t n = std::min(w.size(), static_cast<size_t>(MAX_PATH));
      std::copy_n(w.c_str(), n, dir);
      dir[n] = 0;
    } catch (...) {
      return false;
    }
  }
  // Create the folder and its parent (…\3re\crashes).
  wchar_t parent[MAX_PATH + 1];
  std::copy_n(dir, MAX_PATH + 1, parent);
  if (wchar_t* slash = wcsrchr(parent, L'\\')) {
    *slash = 0;
    CreateDirectoryW(parent, nullptr);
  }
  CreateDirectoryW(dir, nullptr);

  SYSTEMTIME t;
  GetSystemTime(&t);
  wchar_t file[MAX_PATH + 64];
  _snwprintf(file, sizeof file / sizeof file[0] - 1, L"%ls\\crash-%04u%02u%02u-%02u%02u%02u-%03u.txt", dir, t.wYear,
             t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  file[sizeof file / sizeof file[0] - 1] = 0;

  // The module holding the faulting address, and the offset inside it.
  char module[MAX_PATH * 3 + 1] = "?";
  uintptr_t offset = 0;
  HMODULE mod = nullptr;
  if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    static_cast<LPCWSTR>(address), &mod)) {
    wchar_t wmod[MAX_PATH + 1];
    const DWORD len = GetModuleFileNameW(mod, wmod, MAX_PATH);
    wmod[len] = 0;
    WideCharToMultiByte(CP_UTF8, 0, wmod, -1, module, sizeof module, nullptr, nullptr);
    offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(mod);
  }
  const std::string_view ver = version();
  char text[2048];
  const int n = std::snprintf(text, sizeof text,
                              "3re crash report\r\n"
                              "program: %s %.*s\r\n"
                              "time (UTC): %04u-%02u-%02u %02u:%02u:%02u\r\n"
                              "reason: %s\r\n"
                              "exception: 0x%08lX (%s)\r\n"
                              "address: %p\r\n"
                              "module: %s +0x%llX\r\n"
                              "thread: %lu\r\n"
                              "\r\n"
                              "Your work: 3re autosaves open projects; the next start offers to recover them.\r\n",
                              g_app, static_cast<int>(ver.size()), ver.data(), t.wYear, t.wMonth, t.wDay, t.wHour,
                              t.wMinute, t.wSecond, reason ? reason : "?", static_cast<unsigned long>(code),
                              code_name(code), address, module, static_cast<unsigned long long>(offset),
                              static_cast<unsigned long>(GetCurrentThreadId()));
  if (n <= 0) return false;
  const HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(std::min(n, static_cast<int>(sizeof text) - 1));
  const bool ok = WriteFile(h, text, size, &written, nullptr) && written == size;
  CloseHandle(h);
  return ok;
}

#else

void install_crash_handler(std::string_view, const fs::path&) {}
fs::path crash_dir() { return fs::temp_directory_path() / "3re" / "crashes"; }
bool write_crash_report(const char*, uint32_t, const void*) noexcept { return false; }

#endif

std::vector<fs::path> list_crash_reports() {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(crash_dir(), ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (e.is_regular_file(ec) && name.rfind(L"crash-", 0) == 0 && e.path().extension() == L".txt") out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace pixel3d
