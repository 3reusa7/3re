// 3re.exe — the one file a user starts. Runs app\3re-studio.exe (the Qt
// application and its runtime live in app\ so the release root stays clean),
// forwards the command line unchanged, ties the child to a kill-on-close job
// and returns its exit code. THREERE_LAUNCH_TARGET overrides the target (tests);
// THREERE_LAUNCH_NO_UI suppresses the error box. The job lets the updater
// (3re-update.exe) break away, and THREERE_LAUNCHER_PID tells it whom to wait for.
#include <windows.h>

#include <string>

namespace {

// The raw command line after the program name (quotes and spacing preserved).
const wchar_t* argument_tail(const wchar_t* line) {
  if (*line == L'"') {
    ++line;
    while (*line && *line != L'"') ++line;
    if (*line) ++line;
  } else {
    while (*line && *line != L' ' && *line != L'\t') ++line;
  }
  while (*line == L' ' || *line == L'\t') ++line;
  return line;
}

std::wstring env(const wchar_t* name) {
  wchar_t buf[32768];
  const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
  return n && n < 32768 ? std::wstring(buf, n) : std::wstring();
}

std::wstring default_target() {
  wchar_t self[32768];
  const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
  std::wstring dir(self, n);
  dir.resize(dir.find_last_of(L"\\/") + 1);
  return dir + L"app\\3re-studio.exe";
}

int fail(const std::wstring& text) {
  if (env(L"THREERE_LAUNCH_NO_UI").empty()) MessageBoxW(nullptr, text.c_str(), L"3re", MB_OK | MB_ICONERROR);
  return 9;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  std::wstring target = env(L"THREERE_LAUNCH_TARGET");
  if (target.empty()) target = default_target();
  if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
    return fail(L"3re is incomplete: app\\3re-studio.exe is missing.\n\nRe-extract or re-copy the whole 3re folder.");

  std::wstring line = L"\"" + target + L"\"";
  const wchar_t* tail = argument_tail(GetCommandLineW());
  if (*tail) line += L" " + std::wstring(tail);

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
  }
  SetEnvironmentVariableW(L"THREERE_LAUNCHER_PID", std::to_wstring(GetCurrentProcessId()).c_str());
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(target.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
    return fail(L"3re could not start app\\3re-studio.exe (Windows error " + std::to_wstring(GetLastError()) + L").");
  if (job) AssignProcessToJobObject(job, pi.hProcess);
  AllowSetForegroundWindow(pi.dwProcessId);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  if (job) CloseHandle(job);
  return static_cast<int>(code);
}
