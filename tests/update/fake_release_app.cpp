// Stand-in for 3re.exe in the updater tests: "--self-test [--json FILE]" exits 0 when selftest.txt next to
// it says "pass" and 1 otherwise, writing the self-test JSON the way 3re-studio does.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
  const auto dir = std::filesystem::absolute(argv[0]).parent_path();
  std::string verdict;
  std::ifstream(dir / "selftest.txt") >> verdict;
  const bool pass = verdict == "pass";
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--json") == 0)
      std::ofstream(argv[i + 1]) << "{\"passed\": " << (pass ? "true" : "false") << ", \"items\": [{\"name\": \"fake\", \"ok\": "
                                 << (pass ? "true" : "false") << ", \"fatal\": true, \"detail\": \"selftest.txt says " << verdict
                                 << "\"}]}";
  return pass ? 0 : 1;
}
