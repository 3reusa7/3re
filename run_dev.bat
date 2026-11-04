@echo off
rem Development build: configure + build Debug + run all tests (unit tests, then the
rem MCP end-to-end client if Python 3 is installed).
rem Requires the portable toolchain: powershell -File tools\bootstrap_toolchain.ps1 -SkipQt
setlocal
set "ROOT=%~dp0"
set "TC=%ROOT%.toolchain\Qt\Tools"
if not exist "%TC%\mingw1310_64\bin\g++.exe" (
  echo Toolchain missing. Run: powershell -ExecutionPolicy Bypass -File tools\bootstrap_toolchain.ps1 -SkipQt
  exit /b 1
)
set "PATH=%TC%\mingw1310_64\bin;%TC%\CMake_64\bin;%TC%\Ninja;%PATH%"
cd /d "%ROOT%"
cmake --preset debug || exit /b 1
cmake --build --preset debug || exit /b 1
"build\debug\tests\pixel3d-tests.exe" %* || exit /b 1
ctest --preset debug -R mcp-e2e || exit /b 1
echo All tests passed.
