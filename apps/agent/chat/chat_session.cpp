// A conversation: messages in order, the brain's turns and their tool calls, all logged to <dir>/run.jsonl.
#include <fstream>

#include "agent/agent_eval.hpp"
#include "chat.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dchat {

json Message::to_json() const {
  json j = {{"role", role}, {"author", author}};
  if (!text.empty()) j["text"] = text;
  if (!attachments.empty()) {
    json a = json::array();
    for (const auto& x : attachments) a.push_back({{"path", pixel3d::to_utf8(x.path)}, {"kind", x.kind}});
    j["attachments"] = a;
  }
  if (!calls.empty()) {
    json a = json::array();
    for (const auto& c : calls) a.push_back({{"id", c.id}, {"name", c.name}, {"args", c.args}});
    j["calls"] = a;
  }
  if (!call_id.empty()) j["call_id"] = call_id;
  if (!tool.empty()) j["tool"] = tool;
  if (!result.is_null()) j["result"] = result;
  return j;
}

struct Session::Log {
  explicit Log(const fs::path& f) : log(f) {}
  p3dagent::RunLog log;
};

Session::Session(fs::path dir, Provider& brain, Tools& tools) : dir_(std::move(dir)), brain_(brain), tools_(tools) {
  fs::create_directories(dir_);
  // W5: a conversation folder that already has a log is reopened (the app was closed): its messages come back and
  // the log continues (append only).
  const bool reopened = fs::exists(dir_ / "run.jsonl");
  if (reopened) {
    std::ifstream in(dir_ / "run.jsonl", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      json j;
      try {
        j = json::parse(line);
      } catch (...) {
        continue;  // a line cut by a crash
      }
      const std::string ev = j.value("event", std::string());
      if (ev != "message" && ev != "tool_result" && ev != "run_event") continue;
      Message m;
      m.role = j.value("role", std::string());
      m.author = j.value("author", std::string());
      m.text = j.value("text", std::string());
      for (const auto& a : j.value("attachments", json::array()))
        m.attachments.push_back({pixel3d::path_from_utf8(a.value("path", std::string())), a.value("kind", std::string())});
      for (const auto& c : j.value("calls", json::array()))
        m.calls.push_back({c.value("id", std::string()), c.value("name", std::string()), c.value("args", json::object())});
      m.call_id = j.value("call_id", std::string());
      m.tool = j.value("tool", std::string());
      if (j.contains("result")) m.result = j["result"];
      call_no_ += int(m.calls.size());
      messages_.push_back(std::move(m));
    }
  }
  log_ = std::make_unique<Log>(dir_ / "run.jsonl");
  log_->log.append({{"event", reopened ? "conversation_reopened" : "conversation_start"}, {"brain", brain_.author()},
                    {"dir", pixel3d::to_utf8(dir_)}, {"messages", messages_.size()}});
}

Session::~Session() = default;

fs::path Session::log_file() const { return dir_ / "run.jsonl"; }

fs::path Session::current_run() const {
  for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    if (it->role == "tool" && (it->tool == "run_agent" || it->tool == "retexture_with_pictures") && it->result.value("ok", false) &&
        it->result.value("run", json()).is_string())
      return pixel3d::path_from_utf8(it->result["run"].get<std::string>());
  return {};
}

void Session::log(const Message& m) {
  json j = m.to_json();
  j["event"] = m.role == "tool" ? "tool_result" : m.role == "event" ? "run_event" : "message";
  log_->log.append(j);
}

std::vector<Message> Session::user(const std::string& text, const std::vector<Attachment>& attachments, int max_steps) {
  Message m;
  m.role = "user";
  m.author = "you";
  m.text = text;
  m.attachments = attachments;
  for (auto& a : m.attachments)
    if (a.kind.empty()) a.kind = attachment_kind(a.path);
  messages_.push_back(m);
  log(m);
  return loop(messages_.size(), max_steps);
}

std::vector<Message> Session::event(const json& state, int max_steps) {
  Message m;
  m.role = "event";
  m.author = "agent (measured)";
  m.result = state;
  messages_.push_back(m);
  log(m);
  event_turn_ = true;  // nobody answered anything in this turn: answer_decision is refused (see loop)
  auto out = loop(messages_.size() - 1, max_steps);
  event_turn_ = false;
  return out;
}

std::vector<Message> Session::loop(size_t from, int max_steps) {
  const json schemas = tool_schemas();
  for (int step = 0; step < max_steps; ++step) {
    Reply r;
    try {
      r = brain_.next(messages_, schemas);
    } catch (const std::exception& e) {
      r.text = std::string("(the brain failed: ") + e.what() + ")";
    }
    Message a;
    a.role = "assistant";
    a.author = brain_.author();
    a.text = r.text;
    for (auto& c : r.calls) {
      if (c.id.empty()) c.id = "call-" + std::to_string(++call_no_);
      a.calls.push_back(c);
    }
    messages_.push_back(a);
    log(a);
    if (a.calls.empty()) break;
    for (const auto& c : a.calls) {
      log_->log.append({{"event", "tool_call"}, {"id", c.id}, {"name", c.name}, {"args", c.args}, {"by", brain_.author()}});
      Message t;
      t.role = "tool";
      t.author = "agent (measured)";
      t.tool = c.name;
      t.call_id = c.id;
      // Only the person answers the agent's questions: in a turn started by the agent (a run event), the brain may not
      // answer for them (measured: a language-model brain tried to, docs/research/agent-chat-brain.md §7).
      if (event_turn_ && c.name == "answer_decision")
        t.result = {{"ok", false}, {"error", "refused: only the person answers the agent's questions; ask them and wait for their reply"}};
      else
        t.result = tools_.call(c);
      messages_.push_back(t);
      log(t);
    }
  }
  return {messages_.begin() + long(from), messages_.end()};
}

}  // namespace p3dchat
