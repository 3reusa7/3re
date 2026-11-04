// 3re-update.exe — finishes an update after 3re has closed. 3re starts a copy of it from the temp folder
// (a running program's folder cannot be renamed), it waits for 3re to exit, swaps the verified staged
// release in (apps/update/swap.cpp), runs the new release's self-test and, if that fails, puts the previous
// release back. The outcome goes to --result as JSON; 3re shows it at the next start.
//
//   3re-update.exe --install DIR --staged DIR [--wait-pid N]... [--result FILE] [--relaunch]
//   3re-update.exe --rollback --install DIR [--wait-pid N]... [--result FILE] [--relaunch]
//
// The previous release stays next to the install folder as "<install>.previous" (one level of roll-back).
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "update/swap.hpp"

namespace fs = std::filesystem;
using namespace threere::update;

namespace {

struct Args {
  fs::path install, staged, result;
  std::vector<DWORD> pids;
  bool rollback = false, relaunch = false;
  DWORD self_test_ms = 180000;
};

bool parse(int argc, wchar_t** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring k = argv[i];
    auto next = [&]() -> const wchar_t* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (k == L"--install") { if (auto v = next()) a.install = v; else return false; }
    else if (k == L"--staged") { if (auto v = next()) a.staged = v; else return false; }
    else if (k == L"--result") { if (auto v = next()) a.result = v; else return false; }
    else if (k == L"--wait-pid") { if (auto v = next()) a.pids.push_back(DWORD(_wtoi(v))); else return false; }
    else if (k == L"--self-test-ms") { if (auto v = next()) a.self_test_ms = DWORD(_wtoi(v)); else return false; }
    else if (k == L"--rollback") a.rollback = true;
    else if (k == L"--relaunch") a.relaunch = true;
    else return false;
  }
  return !a.install.empty() && (a.rollback || !a.staged.empty());
}

void wait_for_exit(const std::vector<DWORD>& pids) {
  for (const DWORD pid : pids) {
    if (!pid) continue;
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
      WaitForSingleObject(h, 60000);
      CloseHandle(h);
    }
  }
}

// Runs "<install>\3re.exe --self-test --json <file>" without any window; true when it exits 0.
bool self_test(const fs::path& install, DWORD timeout_ms, std::string& detail) {
  const fs::path exe = install / L"3re.exe";
  wchar_t tmp[MAX_PATH + 1];
  GetTempPathW(MAX_PATH, tmp);
  const fs::path report = fs::path(tmp) / (L"3re-update-selftest-" + std::to_wstring(GetCurrentProcessId()) + L".json");
  std::wstring line = L"\"" + exe.wstring() + L"\" --self-test --json \"" + report.wstring() + L"\"";
  SetEnvironmentVariableW(L"THREERE_LAUNCH_NO_UI", L"1");
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, install.c_str(), &si, &pi)) {
    detail = "the new release could not start (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  const DWORD waited = WaitForSingleObject(pi.hProcess, timeout_ms);
  DWORD code = 1;
  if (waited == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    detail = "the new release's self-test did not finish in time";
  } else {
    GetExitCodeProcess(pi.hProcess, &code);
  }
  CloseHandle(pi.hProcess);
  std::ifstream in(report, std::ios::binary);
  if (in) {
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_discarded() && j.contains("items"))
      for (const auto& i : j["items"])
        if (!i.value("ok", false) && i.value("fatal", false))
          detail += (detail.empty() ? "" : "; ") + i.value("name", std::string()) + ": " + i.value("detail", std::string());
  }
  in.close();
  std::error_code ec;
  fs::remove(report, ec);
  if (code != 0 && detail.empty()) detail = "the new release's self-test failed (exit code " + std::to_string(code) + ")";
  return code == 0 && waited != WAIT_TIMEOUT;
}

void relaunch(const fs::path& install) {
  const fs::path exe = install / L"3re.exe";
  std::wstring line = L"\"" + exe.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, install.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

std::string utc_now() {
  SYSTEMTIME t;
  GetSystemTime(&t);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return buf;
}

// Removes a leftover folder from an earlier update; false when it is still there.
bool clear(const fs::path& dir) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return true;
  fs::remove_all(dir, ec);
  return !fs::exists(dir, ec);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Args a;
  if (!parse(argc, argv, a)) return 2;
  wait_for_exit(a.pids);
  const fs::path install = fs::absolute(a.install);
  const fs::path backup = install.wstring() + L".previous";
  nlohmann::json out{{"time_utc", utc_now()}, {"install", u8(install)}};
  int code = 0;

  if (a.rollback) {
    const fs::path aside = install.wstring() + L".rolled-back";
    out["action"] = "rollback";
    out["from"] = release_version(install);
    if (!clear(aside)) {
      out["status"] = "failed";
      out["message"] = "cannot remove " + u8(aside);
      code = 1;
    } else {
      const auto r = roll_back(install, backup, aside);
      out["status"] = r.ok ? "rolled_back" : "failed";
      out["message"] = r.message;
      out["to"] = release_version(install);
      code = r.ok ? 0 : 1;
    }
  } else {
    out["action"] = "update";
    out["from"] = release_version(install);
    out["to"] = release_version(a.staged);
    if (!clear(backup)) {
      out["status"] = "failed";
      out["message"] = "cannot remove the older backup " + u8(backup) + " (is a file in it open?)";
      code = 1;
    } else if (const auto r = swap_in(install, a.staged, backup); !r.ok) {
      out["status"] = "failed";
      out["message"] = r.message;  // nothing changed
      code = 1;
    } else {
      out["carried"] = r.carried;
      std::string detail;
      if (self_test(install, a.self_test_ms, detail)) {
        out["status"] = "updated";
        out["message"] = r.message;
      } else {
        const fs::path failed = install.wstring() + L".failed";
        clear(failed);
        const auto back = roll_back(install, backup, failed);
        out["status"] = back.ok ? "rolled_back" : "failed";
        out["message"] = "the new release failed its self-test (" + detail + "); " +
                         (back.ok ? "the previous version was restored" : "restoring the previous version failed: " + back.message);
        code = 1;
      }
    }
  }
  if (!a.result.empty()) {
    std::error_code ec;
    fs::create_directories(a.result.parent_path(), ec);
    std::ofstream(a.result, std::ios::binary | std::ios::trunc) << out.dump(1);
  }
  if (a.relaunch) relaunch(install);
  return code;
}
