// Pixel3D-mcp — stdio MCP server over pixel3d-core (spec 2026-10-07-pixel3d-m2-mcp.md).
//
// stdout belongs to the protocol alone: at start-up the real stdout is
// duplicated for JSON-RPC output and file descriptor 1 is pointed at stderr, so
// any stray output from the engine or a library lands in the log, never in the
// protocol stream. A reader thread takes lines from stdin (and handles
// cancellation immediately); requests run in order on the main thread.
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

#include "mcp/server.hpp"
#include "pixel3d/common/crash_report.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/common/version.hpp"

using namespace pixel3d;
using namespace pixel3d::mcp;

namespace {

std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  for (int i = 1; w && i < n; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, len - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
    out.push_back(std::move(s));
  }
  if (w) LocalFree(w);
#else
  for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
  return out;
}

const char kUsage[] =
    "3re-mcp - Model Context Protocol server (stdio) for 3re\n"
    "\n"
    "  3re-mcp [--root DIR]... [--log FILE] [--quiet]\n"
    "\n"
    "  --root DIR   only allow file paths inside DIR (repeatable; default: any path)\n"
    "  --log FILE   also append the log to FILE\n"
    "  --quiet      log warnings and errors only\n"
    "  --version    print the version\n"
    "\n"
    "Speaks JSON-RPC 2.0 on stdin/stdout, one message per line. Logs go to stderr.\n"
    "Client configuration example: {\"command\": \"C:/path/3re/app/3re-mcp.exe\", \"args\": []}\n";

// The protocol output stream (the original stdout).
class ProtocolOut {
 public:
  ProtocolOut() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::fflush(stdout);
    const int fd = _dup(_fileno(stdout));
    out_ = fd >= 0 ? _fdopen(fd, "wb") : nullptr;
    if (_fileno(stderr) >= 0) _dup2(_fileno(stderr), _fileno(stdout));
    else std::freopen("NUL", "wb", stdout);
#else
    std::fflush(stdout);
    const int fd = dup(fileno(stdout));
    out_ = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    dup2(fileno(stderr), fileno(stdout));
#endif
  }
  bool ok() const { return out_ != nullptr; }
  void write(const std::string& line) {
    std::lock_guard lock(mu_);
    std::fwrite(line.data(), 1, line.size(), out_);
    std::fputc('\n', out_);
    std::fflush(out_);
  }

 private:
  std::FILE* out_ = nullptr;
  std::mutex mu_;
};

class Log {
 public:
  void open(const std::string& path) { file_.open(path_from_utf8(path), std::ios::app | std::ios::binary); }
  void set_quiet(bool q) { quiet_ = q; }
  void operator()(const char* level, const std::string& text) {
    if (quiet_ && (std::strcmp(level, "info") == 0 || std::strcmp(level, "debug") == 0)) return;
    using namespace std::chrono;
    const auto now = floor<milliseconds>(system_clock::now());
    const auto t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    const std::string line = std::string(ts) + "Z pixel3d-mcp " + level + ": " + text + "\n";
    std::lock_guard lock(mu_);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
    if (file_) file_ << line << std::flush;
  }

 private:
  std::ofstream file_;
  bool quiet_ = false;
  std::mutex mu_;
};

// Lines from stdin, bounded in size; nullopt at end of input.
class LineQueue {
 public:
  void push(std::optional<std::string> line) {
    {
      std::lock_guard lock(mu_);
      q_.push_back(std::move(line));
    }
    cv_.notify_one();
  }
  std::optional<std::string> pop() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return !q_.empty(); });
    auto v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::optional<std::string>> q_;
};

void read_stdin(LineQueue& q, Server& server, Log& log) {
  std::string line;
  bool too_long = false;
  char buf[65536];
  while (true) {
    // Low-level read: returns as soon as any bytes arrive (fread would wait for a full buffer).
#ifdef _WIN32
    const int got = _read(_fileno(stdin), buf, sizeof buf);
#else
    const ssize_t got = read(fileno(stdin), buf, sizeof buf);
#endif
    if (got <= 0) break;
    const size_t n = static_cast<size_t>(got);
    for (size_t i = 0; i < n; ++i) {
      const char c = buf[i];
      if (c != '\n') {
        if (line.size() < kMaxMessageBytes) line.push_back(c);
        else too_long = true;
        continue;
      }
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (too_long) {
        log("error", "dropped a message larger than the limit");
        q.push(std::string("\x01too-long"));
      } else if (auto id = Server::cancellation_target(line)) {
        server.cancel(*id);  // takes effect now, even while a request runs
      } else {
        q.push(std::move(line));
      }
      line.clear();
      too_long = false;
    }
  }
  if (!line.empty() && !too_long) q.push(std::move(line));
  q.push(std::nullopt);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Log lines and --help are UTF-8 (paths in any script); the protocol stream is bytes either way.
  SetConsoleOutputCP(CP_UTF8);
#endif
  install_crash_handler("3re-mcp");
  const std::vector<std::string> args = utf8_args(argc, argv);
  ServerOptions opt;
  Log log;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--version") {
      std::printf("3re-mcp %s\n", std::string(version()).c_str());
      return 0;
    }
    if (args[i] == "--help" || args[i] == "-h") {
      std::fputs(kUsage, stdout);
      return 0;
    }
    if (args[i] == "--quiet") {
      log.set_quiet(true);
    } else if ((args[i] == "--root" || args[i] == "--log") && i + 1 < args.size()) {
      if (args[i] == "--root") opt.paths.roots.push_back(path_from_utf8(args[++i]));
      else log.open(args[++i]);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n\n%s", args[i].c_str(), kUsage);
      return 2;
    }
  }

  ProtocolOut out;  // from here on, stdout is the protocol stream only
  if (!out.ok()) {
    std::fprintf(stderr, "pixel3d-mcp: cannot open the protocol output\n");
    return 1;
  }
  opt.log = [&log](const char* level, const std::string& text) { log(level, text); };
  Server server(opt, [&out](const std::string& line) { out.write(line); });
  log("info", "3re-mcp " + std::string(version()) + " ready on stdio" +
                  (opt.paths.roots.empty() ? "" : " (path roots restricted)"));

  LineQueue queue;
  std::thread reader(read_stdin, std::ref(queue), std::ref(server), std::ref(log));
  while (auto line = queue.pop()) {
    if (*line == "\x01too-long") {
      out.write(to_line(rpc_error(nullptr, rpc::kInvalidRequest, "Invalid Request: message exceeds the size limit")));
      continue;
    }
    server.handle_line(*line);
  }
  reader.join();
  log("info", "input closed; exiting");
  return 0;
}
