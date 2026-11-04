// 3re-agent.exe spec.json [--out DIR] [--config 3re.config.json] [--app-dir DIR] [--workers DIR]...
//                [--godot GODOT_CONSOLE_EXE|none] [--validator gltf_validator.exe] [--no-hash-weights]
//                [--ask [--wait-seconds N]] [--answers answers.json]
// 3re-agent.exe --answer RUN_DIR QUESTION_ID CHOICE [--picture FILE] [--label back|left|right|...] [--who NAME]
// The Game-Ready Asset Agent (docs/agent/PLAN.md, PLAN-M3.md). Without --ask the agent never waits: a question is
// written to the report as PENDING_DECISION. Exit: 0 PASS or ACCEPTED, 1 REVIEW/UNVERIFIED, 2 FAIL, 3 BLOCKED,
// 4 spec/usage/output-folder error (--answer: 0 written, 4 refused).
#include <windows.h>

#include <cstdio>
#include <string>

#include "agent/agent_decide.hpp"
#include "agent/agent_eval.hpp"
#include "agent/agent_exec.hpp"
#include "agent/agent_run.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace {

std::string user_name() {
  wchar_t buf[256];
  const DWORD n = GetEnvironmentVariableW(L"USERNAME", buf, 256);
  return n && n < 256 ? p3dagent::narrow(std::wstring(buf, n)) : std::string("unknown user");
}

int answer_main(int argc, wchar_t** argv) {
  // argv[1] == --answer
  if (argc < 5) {
    std::fprintf(stderr, "usage: 3re-agent --answer RUN_DIR QUESTION_ID CHOICE [--picture FILE] [--label LABEL] [--who NAME]\n");
    return 4;
  }
  p3dagent::Answer a;
  const std::filesystem::path run = argv[2];
  a.id = p3dagent::narrow(argv[3]);
  a.choice = p3dagent::narrow(argv[4]);
  a.who = user_name();
  a.when = p3dagent::now_iso();
  a.source = "cli";
  for (int i = 5; i < argc; ++i) {
    const std::wstring k = argv[i];
    if (i + 1 >= argc) return 4;
    const std::wstring v = argv[++i];
    if (k == L"--picture") a.picture = pixel3d::to_utf8(std::filesystem::absolute(std::filesystem::path(v)));
    else if (k == L"--label") a.label = p3dagent::narrow(v);
    else if (k == L"--who") a.who = p3dagent::narrow(v);
    else return 4;
  }
  std::string error;
  if (!p3dagent::write_answer_file(run, a, error)) {
    std::fprintf(stderr, "answer refused: %s\n", error.c_str());
    return 4;
  }
  std::printf("answer written: %s %s (%s, %s)\n", a.id.c_str(), a.choice.c_str(), a.who.c_str(), a.when.c_str());
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  if (argc >= 2 && std::wstring(argv[1]) == L"--answer") return answer_main(argc, argv);
  p3dagent::AgentOptions o;
  auto usage = [] {
    std::fprintf(stderr,
                 "usage: 3re-agent spec.json [--out DIR] [--config 3re.config.json] [--app-dir DIR] [--workers DIR]...\n"
                 "                 [--godot GODOT_CONSOLE_EXE|none] [--validator gltf_validator.exe] [--no-hash-weights]\n"
                 "                 [--ask [--wait-seconds N]] [--answers answers.json]\n"
                 "       3re-agent --answer RUN_DIR QUESTION_ID CHOICE [--picture FILE] [--label LABEL] [--who NAME]\n");
    return 4;
  };
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> std::filesystem::path { return i + 1 < argc ? std::filesystem::path(argv[++i]) : std::filesystem::path(); };
    if (a == L"--out") o.out_dir = next();
    else if (a == L"--config") o.config = next();
    else if (a == L"--app-dir") o.app_dir = next();
    else if (a == L"--workers") o.workers.push_back(next());
    else if (a == L"--godot") o.godot = next();
    else if (a == L"--validator") o.validator = next();
    else if (a == L"--no-hash-weights") o.hash_weights = false;
    else if (a == L"--ask") o.decisions.mode = p3dagent::AskMode::Ask;
    else if (a == L"--wait-seconds") {
      try {
        o.decisions.wait_seconds = std::stod(next().wstring());
      } catch (...) {
        return usage();
      }
      if (!(o.decisions.wait_seconds >= 0)) return usage();
    } else if (a == L"--answers") o.decisions.answers_file = next();
    else if (a == L"--help" || a == L"-h") return usage();
    else if (!a.empty() && a[0] != L'-' && o.spec_file.empty()) o.spec_file = a;
    else return usage();
  }
  if (o.spec_file.empty()) return usage();
  return p3dagent::run_agent(o);
}
