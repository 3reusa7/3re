#include "pixel3d/engine/history.hpp"

namespace pixel3d {

void to_json(nlohmann::json& j, const LogEntry& e) {
  j = nlohmann::json{{"time", e.time},
                     {"source", to_string(e.source)},
                     {"name", e.name},
                     {"params", e.params},
                     {"summary", e.summary},
                     {"success", e.success}};
}

History::History() : states_{Document{}}, steps_{"start"} {}

void History::push(Document d, std::string step) {
  states_.resize(cursor_ + 1);
  steps_.resize(cursor_ + 1);
  states_.push_back(std::move(d));
  steps_.push_back(std::move(step));
  cursor_ = states_.size() - 1;
}

const Document* History::undo() {
  if (!can_undo()) return nullptr;
  return &states_[--cursor_];
}

const Document* History::redo() {
  if (!can_redo()) return nullptr;
  return &states_[++cursor_];
}

void History::checkpoint(const std::string& name) { checkpoints_[name] = current(); }

std::optional<Document> History::checkpoint_state(const std::string& name) const {
  const auto it = checkpoints_.find(name);
  if (it == checkpoints_.end()) return std::nullopt;
  return it->second;
}

std::vector<std::string> History::checkpoint_names() const {
  std::vector<std::string> n;
  for (const auto& [k, v] : checkpoints_) n.push_back(k);
  return n;
}

}  // namespace pixel3d
