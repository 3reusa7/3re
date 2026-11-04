// 3re-chat.exe --dir CONVERSATION_DIR [--script FILE] [--runs DIR] [--agent 3re-agent.exe] [--projects DIR] [--agent-arg ARG]... [--wait-seconds N] [--continue]
//        3re-chat.exe --eval requests.json   (brain evaluation set 1: request -> spec fields, rules brain)
//        3re-chat.exe --eval-brain EVAL_DIR OUT_DIR [--brain rules|http:HOST:PORT:MODEL] [--set2 DIR]   (sets 1-3)
// --brain http:127.0.0.1:8080:qwen3-vl-4b uses a language model behind llama-server (default: rules).
// The chat agent without the window (milestone 4, W3): the same brain, tools and log as the app's chat panel.
// Script (UTF-8, one item per line): "you: text", "attach: path" (attaches to the next "you:" line), "wait" (wait until
// the run asks a question or finishes, then hand that to the brain), "# comment". Without --script: interactive.
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "agent/agent_exec.hpp"
#include "chat.hpp"
#include "chat_eval.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace {

using p3dchat::json;
namespace fs = std::filesystem;

void print(const std::vector<p3dchat::Message>& ms) {
  for (const auto& m : ms) {
    if (m.role == "assistant") {
      if (!m.text.empty()) std::cout << "[" << m.author << "] " << m.text << "\n";
      for (const auto& c : m.calls) std::cout << "  -> tool " << c.name << " " << c.args.dump() << "\n";
    } else if (m.role == "tool") {
      std::cout << "  <- " << m.tool << ": " << m.result.dump().substr(0, 600) << "\n";
    } else if (m.role == "event") {
      std::cout << "  <- run " << m.result.value("state", "") << "\n";
    }
  }
  std::cout << std::flush;
}

// Waits until the run asks a question or finishes (or the time is up). Returns its state.
json wait_run(const fs::path& run, double seconds) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    json s = p3dchat::read_run_state(run);
    const std::string st = s.value("state", "");
    if (st == "waiting" || st == "finished") return s;
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return s;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

}  // namespace

// Brain evaluation set 1 (docs/agent/chat-eval/requests.json): each request read by the rules brain, every field
// compared with the expected one; fields the request does not give must stay unset; "ask" = the first field the
// brain asks for (same order as RulesProvider). Prints one line per request and the score.
int run_eval(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  const json set = json::parse(in);
  int correct = 0, total = 0;
  for (const auto& r : set["requests"]) {
    p3dchat::Draft d;
    p3dchat::read_words(r["text"].get<std::string>(), d);
    json got = json::object();
    if (d.asset_type) got["asset_type"] = *d.asset_type;
    if (d.platform) got["platform"] = *d.platform;
    if (d.triangle_budget) got["triangle_budget"] = *d.triangle_budget;
    if (d.metres) got["metres"] = *d.metres;
    if (d.texture_size) got["texture_size"] = *d.texture_size;
    if (!d.style.empty()) got["style"] = d.style;
    if (!d.engine_words.empty()) got["engine_words"] = d.engine_words;
    if (d.wants_rigging) got["wants_rigging"] = true;
    if (d.wants_status) got["wants_status"] = true;
    // What the brain asks first (pictures assumed attached): type, then platform or budget, then size.
    const bool request = d.asset_type || d.platform || d.triangle_budget || d.metres || !d.wants_status;
    if (request && !d.wants_status) {
      if (!d.asset_type) got["ask"] = "asset_type";
      else if (!d.platform && !d.triangle_budget) got["ask"] = "platform";
      else if (!d.metres) got["ask"] = "metres";
    }
    json expect = r["expect"];
    bool ok = true;
    std::string why;
    for (const auto& [k, v] : expect.items()) {
      const bool same = got.contains(k) && (v.is_number() ? std::abs(got[k].get<double>() - v.get<double>()) < 1e-9 : got[k] == v);
      if (!same) ok = false, why += " " + k + ": expected " + v.dump() + ", got " + (got.contains(k) ? got[k].dump() : "nothing") + ";";
    }
    for (const auto& [k, v] : got.items())
      if (!expect.contains(k)) ok = false, why += " " + k + ": not asked for, got " + v.dump() + ";";
    ++total;
    correct += ok;
    std::cout << (ok ? "OK   " : "MISS ") << r["text"].get<std::string>() << (ok ? "" : " —" + why) << "\n";
  }
  std::cout << "request -> spec fields (rules brain): " << correct << " / " << total << "\n";
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
  if (argc == 3 && std::wstring(argv[1]) == L"--eval") return run_eval(argv[2]);
  // The brain: rules (default) or a local model server.
  std::string brain_spec = "rules";
  for (int i = 1; i + 1 < argc; ++i)
    if (std::wstring(argv[i]) == L"--brain") brain_spec = p3dagent::narrow(argv[i + 1]);
  std::unique_ptr<p3dchat::Provider> brain;
  if (brain_spec.rfind("http:", 0) == 0) {
    // http:HOST:PORT:MODEL
    const auto a = brain_spec.find(':', 5), b = a == std::string::npos ? a : brain_spec.find(':', a + 1);
    if (b == std::string::npos) {
      std::fprintf(stderr, "--brain http:HOST:PORT:MODEL\n");
      return 4;
    }
    brain = std::make_unique<p3dchat::HttpProvider>(brain_spec.substr(5, a - 5), std::stoi(brain_spec.substr(a + 1, b - a - 1)), brain_spec.substr(b + 1));
  } else {
    brain = std::make_unique<p3dchat::RulesProvider>();
  }
  if (argc >= 4 && std::wstring(argv[1]) == L"--eval-brain") {
    fs::path set2;
    for (int i = 1; i + 1 < argc; ++i)
      if (std::wstring(argv[i]) == L"--set2") set2 = argv[i + 1];
    return p3dchat::run_brain_eval(*brain, argv[2], argv[3], set2);
  }
  fs::path dir, script, runs, agent, projects;
  std::vector<std::string> agent_args;
  double wait_seconds = 3600;
  bool resume = false;  // --continue: reopen the conversation in --dir (its log is appended to)
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> fs::path { return i + 1 < argc ? fs::path(argv[++i]) : fs::path(); };
    if (a == L"--dir") dir = next();
    else if (a == L"--script") script = next();
    else if (a == L"--runs") runs = next();
    else if (a == L"--agent") agent = next();
    else if (a == L"--projects") projects = next();
    else if (a == L"--continue") resume = true;
    else if (a == L"--brain") next();  // read above
    else if (a == L"--agent-arg") agent_args.push_back(p3dagent::narrow(next().wstring()));
    else if (a == L"--wait-seconds") wait_seconds = std::stod(next().wstring());
    else {
      std::fprintf(stderr, "usage: 3re-chat --dir CONVERSATION_DIR [--script FILE] [--runs DIR] [--agent 3re-agent.exe] [--wait-seconds N]\n");
      return 4;
    }
  }
  if (dir.empty()) {
    std::fprintf(stderr, "--dir is required\n");
    return 4;
  }
  if (fs::exists(dir / "run.jsonl") && !resume) {
    std::fprintf(stderr, "the conversation folder already has a run.jsonl (nothing is ever overwritten): %s\n", pixel3d::to_utf8(dir).c_str());
    return 4;
  }
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (agent.empty()) agent = fs::path(self).parent_path() / "3re-agent.exe";
  if (runs.empty()) runs = dir / "runs";
  wchar_t user[256];
  const DWORD n = GetEnvironmentVariableW(L"USERNAME", user, 256);

  if (projects.empty()) {
    wchar_t home[MAX_PATH];
    const DWORD h = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (h && h < MAX_PATH) projects = fs::path(std::wstring(home, h)) / L"Documents" / L"3re" / L"Projects";
  }
  p3dchat::ToolEnv env{dir, runs, agent, n && n < 256 ? p3dagent::narrow(std::wstring(user, n)) : std::string("unknown user"), projects, {}};
  env.agent_args = agent_args;
  p3dchat::Tools tools(env);
  p3dchat::Session session(dir, *brain, tools);
  std::cout << "3re chat — brain: " << brain->author() << " — log: " << pixel3d::to_utf8(session.log_file()) << "\n";

  std::vector<p3dchat::Attachment> pending;
  auto handle = [&](const std::string& line) {
    if (line.empty() || line[0] == '#') return;
    if (line.rfind("attach:", 0) == 0) {
      std::string p = line.substr(7);
      while (!p.empty() && p.front() == ' ') p.erase(p.begin());
      const fs::path f = pixel3d::path_from_utf8(p);
      pending.push_back({f, p3dchat::attachment_kind(f)});
      return;
    }
    if (line == "wait") {
      const fs::path run = session.current_run();
      if (run.empty()) {
        std::cout << "(no run to wait for)\n";
        return;
      }
      std::cout << "... waiting for the run (question or result)\n" << std::flush;
      print(session.event(wait_run(run, wait_seconds)));
      return;
    }
    std::string text = line.rfind("you:", 0) == 0 ? line.substr(4) : line;
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    std::cout << "[you] " << text;
    for (const auto& a : pending) std::cout << " +" << pixel3d::to_utf8(a.path.filename());
    std::cout << "\n";
    print(session.user(text, pending));
    pending.clear();
  };
  if (!script.empty()) {
    std::ifstream in(script, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "cannot read %s\n", pixel3d::to_utf8(script).c_str());
      return 4;
    }
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // BOM
      handle(line);
    }
  } else {
    std::string line;
    while (std::cout << "> " << std::flush, std::getline(std::cin, line)) handle(line);
  }
  return 0;
}
