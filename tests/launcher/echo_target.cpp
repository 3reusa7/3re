// Test stand-in for app\3re-studio.exe: writes its raw command-line tail to
// %THREERE_ECHO_OUT% and exits with code 7, so the launcher's forwarding is observable.
#include <windows.h>

#include <cstdio>

int main() {
  const wchar_t* line = GetCommandLineW();
  const wchar_t* tail = line;
  if (*tail == L'"') {
    ++tail;
    while (*tail && *tail != L'"') ++tail;
    if (*tail) ++tail;
  } else {
    while (*tail && *tail != L' ' && *tail != L'\t') ++tail;
  }
  while (*tail == L' ' || *tail == L'\t') ++tail;
  wchar_t out[MAX_PATH];
  if (!GetEnvironmentVariableW(L"THREERE_ECHO_OUT", out, MAX_PATH)) return 2;
  FILE* f = _wfopen(out, L"wb");
  if (!f) return 3;
  const int n = WideCharToMultiByte(CP_UTF8, 0, tail, -1, nullptr, 0, nullptr, nullptr);
  char* utf8 = new char[n];
  WideCharToMultiByte(CP_UTF8, 0, tail, -1, utf8, n, nullptr, nullptr);
  std::fputs(utf8, f);
  std::fclose(f);
  delete[] utf8;
  return 7;
}
