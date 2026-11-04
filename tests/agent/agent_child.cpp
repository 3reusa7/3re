// Test child for the agent's process runner: `agent-child exit N | sleep MS | alloc MIB | echo ARG...`.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) return 100;
  if (!std::strcmp(argv[1], "exit") && argc == 3) return std::atoi(argv[2]);
  if (!std::strcmp(argv[1], "sleep") && argc == 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
    return 0;
  }
  if (!std::strcmp(argv[1], "alloc") && argc == 3) {
    // Commit and touch MiB by MiB; a job memory limit makes an allocation fail.
    std::vector<char*> blocks;
    try {
      for (int i = 0; i < std::atoi(argv[2]); ++i) {
        char* b = new char[1 << 20];
        std::memset(b, 1, 1 << 20);
        blocks.push_back(b);
      }
    } catch (const std::bad_alloc&) {
      std::fprintf(stderr, "out of memory after %zu MiB\n", blocks.size());
      return 3;
    }
    for (char* b : blocks) delete[] b;
    return 0;
  }
  if (!std::strcmp(argv[1], "echo")) {
    for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
    return 0;
  }
  return 101;
}
