// M5 crash reports: a report is a small text file with what the user (or a
// developer) needs, written into the crash folder and listed oldest first.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "pixel3d/common/crash_report.hpp"
#include "pixel3d/common/version.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

#ifdef _WIN32
TEST_CASE("crash report: written to the crash folder with code, module and version") {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_crash_test" / "crashes";
  fs::remove_all(dir.parent_path());
  install_crash_handler("pixel3d-tests", dir);
  CHECK(crash_dir() == dir);
  CHECK(list_crash_reports().empty());
  // An address inside this program: the report names the module and the offset.
  const void* address = reinterpret_cast<const void*>(&list_crash_reports);
  REQUIRE(write_crash_report("test report", 0xC0000005u, address));
  const auto reports = list_crash_reports();
  REQUIRE(reports.size() == 1);
  CHECK(reports[0].filename().string().rfind("crash-", 0) == 0);
  const std::string text = [&] {
    std::ifstream in(reports[0], std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }();
  CHECK(text.find("program: pixel3d-tests " + std::string(version())) != std::string::npos);
  CHECK(text.find("reason: test report") != std::string::npos);
  CHECK(text.find("0xC0000005 (access violation)") != std::string::npos);
  CHECK(text.find("pixel3d-tests.exe +0x") != std::string::npos);
  fs::remove_all(dir.parent_path());
}
#endif
