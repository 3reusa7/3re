#pragma once
// Linear undo/redo over Document snapshots, plus named checkpoints and the
// mutation log. Restoring a checkpoint is itself an undoable step.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/document.hpp"

namespace pixel3d {

struct LogEntry {
  std::string time;  // ISO 8601 UTC
  Source source = Source::Cli;
  std::string name;
  nlohmann::json params;
  std::string summary;
  bool success = false;
};

void to_json(nlohmann::json& j, const LogEntry& e);

class History {
 public:
  History();
  const Document& current() const { return states_[cursor_]; }
  void push(Document d, std::string step = {});  // drops the redo branch
  // Derived data only (validation, analysis): updates the current snapshot
  // without creating an undo step.
  void replace_current(Document d) { states_[cursor_] = std::move(d); }
  bool can_undo() const { return cursor_ > 0; }
  bool can_redo() const { return cursor_ + 1 < states_.size(); }
  const Document* undo();
  const Document* redo();
  void checkpoint(const std::string& name);
  void set_checkpoint(const std::string& name, Document d) { checkpoints_[name] = std::move(d); }
  void erase_checkpoint(const std::string& name) { checkpoints_.erase(name); }
  std::optional<Document> checkpoint_state(const std::string& name) const;
  std::vector<std::string> checkpoint_names() const;
  void set_checkpoints(std::map<std::string, Document> cps) { checkpoints_ = std::move(cps); }
  const std::map<std::string, Document>& checkpoints() const { return checkpoints_; }
  // The undo list: the name of the operation that produced each state (state 0
  // is the start), and which state is current.
  const std::vector<std::string>& steps() const { return steps_; }
  size_t cursor() const { return cursor_; }

 private:
  std::vector<Document> states_;
  std::vector<std::string> steps_;
  size_t cursor_ = 0;
  std::map<std::string, Document> checkpoints_;
};

}  // namespace pixel3d
