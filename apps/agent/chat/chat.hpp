#pragma once
// 3re chat agent (milestone 4, W3; docs/agent/PLAN-M4.md). Qt-free so the app, 3re-chat.exe and the tests share it.
//
// A conversation is a run: <dir>/run.jsonl logs every message, tool call and tool result. The brain (a Provider)
// plans and explains; it never decides acceptance: every PASS / REVIEW / FAIL it repeats comes from the agent's
// report.json and is marked as measured. Providers: "rules" (deterministic, offline, always available), "fake"
// (scripted, tests) and "http" (an OpenAI-compatible local server or a cloud API, optional; chat_http.cpp).
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace p3dchat {

namespace fs = std::filesystem;
using nlohmann::json;

struct Attachment {
  fs::path path;
  std::string kind;  // "picture" | "glb" | "unsupported"
};
std::string attachment_kind(const fs::path& p);

struct ToolCall {
  std::string id, name;
  json args = json::object();
};

// role: "user" | "assistant" | "tool" (one tool result) | "event" (the agent run changed: question, finished).
// author: who wrote it — "you", "rules", "model:<name>", "fake", or "agent (measured)" for tool results and events.
struct Message {
  std::string role, text, author;
  std::vector<Attachment> attachments;
  std::vector<ToolCall> calls;  // assistant
  std::string call_id, tool;    // tool
  json result;                  // tool / event
  json to_json() const;
};

struct Reply {
  std::string text;
  std::vector<ToolCall> calls;
};

class Provider {
 public:
  virtual ~Provider() = default;
  virtual std::string author() const = 0;  // "rules", "model:qwen3-vl-4b", ...
  // The next assistant turn for the conversation so far. `tools` = tool_schemas().
  virtual Reply next(const std::vector<Message>& conversation, const json& tools) = 0;
};

// Deterministic brain: Arabic and English words and numbers → spec fields; asks for each missing required field;
// calls the tools in a fixed order. Reads no pictures (side labels come from file names or the registration).
class RulesProvider : public Provider {
 public:
  std::string author() const override { return "rules"; }
  Reply next(const std::vector<Message>& conversation, const json& tools) override;
};

// A language model behind an OpenAI-compatible endpoint on this machine (llama.cpp llama-server --jinja): chat_http.cpp.
class HttpProvider : public Provider {
 public:
  HttpProvider(std::string host, int port, std::string model, int picture_side = 448, int timeout_seconds = 300);
  std::string author() const override { return "model:" + model_; }
  Reply next(const std::vector<Message>& conversation, const json& tools) override;
  double last_seconds() const { return last_seconds_; }
  json last_usage() const { return last_usage_; }

 private:
  std::string host_;
  int port_;
  std::string model_;
  int side_, timeout_;
  double last_seconds_ = 0;
  json last_usage_;
};

// Scripted replies for tests (a provider that is not a model and never pretends to be one).
class FakeProvider : public Provider {
 public:
  explicit FakeProvider(std::deque<Reply> script) : script_(std::move(script)) {}
  std::string author() const override { return "fake"; }
  Reply next(const std::vector<Message>&, const json&) override;
  size_t remaining() const { return script_.size(); }

 private:
  std::deque<Reply> script_;
};

// What a person asked for, as far as words and attachments say (the rules brain's reading; also a tool argument).
struct Draft {
  std::optional<std::string> asset_type;  // "character" | "prop"
  std::optional<std::string> platform;    // "mobile" | "pc" | "hero"
  std::optional<int> triangle_budget, texture_size;
  std::optional<double> metres;
  std::optional<std::string> axis;        // "height" | "longest"
  std::string style;                      // requested style words ("" = none)
  std::string engine_words;               // an engine other than Godot that was named
  bool wants_rigging = false, wants_status = false, wants_report = false, wants_cancel = false;
  std::optional<json> mark;               // "mark yaw=0 x=235 y=117 r=7" (from the app's marking or typed)
  std::vector<Attachment> pictures, glbs;
  json to_json() const;
};
// Reads one message into the draft (later messages override earlier values).
void read_words(const std::string& text, Draft& d);
bool has_arabic(const std::string& utf8);
// Arabic-Indic and Eastern Arabic-Indic digits → ASCII, Arabic decimal/thousands separators → '.' / ','.
std::string ascii_digits(const std::string& utf8);

// The tools the brain may call. Each call and its result are logged by the Session.
json tool_schemas();

struct ToolEnv {
  fs::path conversation_dir;  // specs and the conversation's run.jsonl live here
  fs::path runs_dir;          // agent runs started from the chat
  fs::path agent_exe;         // 3re-agent.exe
  std::string who;            // the person answering questions (Windows user name)
  fs::path projects_dir;      // 3re projects (Documentsre\Projects): a saved generation of the same picture is reused
  // Starts the agent (tests replace it). Default: a detached 3re-agent process that survives the app (W5).
  std::function<bool(const fs::path& spec, const fs::path& out, std::string& error)> start_agent;
  // W5: free RAM (MiB) and other 3re-agent processes, measured before a run starts (tests replace them).
  std::function<uint64_t()> free_ram_mib;
  std::function<int()> agents_running;
  std::vector<std::string> agent_args;  // extra 3re-agent arguments (development runs: --config, --workers, --godot, ...)
};

// W5 admission rule for one agent run (measured numbers in, decision and reason out).
struct Admission {
  bool start = false;
  std::string reason;
  uint64_t needed_mib = 0;
};
Admission admit_run(bool generates, uint64_t free_mib, int agents_running);
// Measured now: running 3re-agent.exe processes, free physical RAM (MiB).
int count_agent_processes();
uint64_t free_ram_mib();

class Tools {
 public:
  explicit Tools(ToolEnv env);
  json call(const ToolCall& c);  // {"ok": bool, ...}; never throws
  const ToolEnv& env() const { return env_; }

  // Tool implementations (public for tests).
  json make_spec(const json& a);
  json validate_spec(const json& a);
  json propose_labels(const json& a);
  json run_agent(const json& a);
  json run_status(const json& a);
  json answer_decision(const json& a);
  json open_report(const json& a);
  json region_views(const json& a);
  json regional_fix(const json& a);
  json retexture_with_pictures(const json& a);

 private:
  ToolEnv env_;
  bool started_by_test_ = false;  // the process start was supplied (tests): no executable to look for
};

// State of an agent run read from its folder (run.jsonl, decisions/, report.json): nothing is inferred.
json read_run_state(const fs::path& run_dir);

class Session {
 public:
  Session(fs::path dir, Provider& brain, Tools& tools);
  ~Session();
  // A person's message; runs the brain and its tool calls (at most `max_steps` assistant turns) and returns the new
  // messages (assistant, tool, assistant...).
  std::vector<Message> user(const std::string& text, const std::vector<Attachment>& attachments = {}, int max_steps = 8);
  // The agent run changed (a question appeared, it finished): the event goes to the brain like a tool result.
  std::vector<Message> event(const json& state, int max_steps = 4);
  const std::vector<Message>& messages() const { return messages_; }
  // The run this conversation started last ("" = none).
  fs::path current_run() const;
  fs::path log_file() const;

 private:
  std::vector<Message> loop(size_t from, int max_steps);
  void log(const Message& m);
  fs::path dir_;
  Provider& brain_;
  Tools& tools_;
  std::vector<Message> messages_;
  struct Log;
  std::unique_ptr<Log> log_;
  int call_no_ = 0;
  bool event_turn_ = false;
};

}  // namespace p3dchat
