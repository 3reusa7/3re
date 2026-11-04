#pragma once
// Agent milestone 3 — human-in-the-loop decisions (docs/agent/PLAN-M3.md). The agent asks instead of failing
// silently: a question names what it measured, why it cannot decide alone and the options; a person answers
// (the app's run viewer, `3re-agent --answer`, or an answers file written beforehand). Headless runs never answer
// themselves: the question is written to the report as PENDING_DECISION and the run keeps its measured result.
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

struct QuestionOption {
  std::string id, label, detail;
  bool needs_picture = false;  // the answer must carry a picture file and its angle label
};

struct Question {
  std::string id;      // q1, q2, ... in the order asked
  std::string kind;    // "budget_or_fidelity" | "hidden_side_picture"
  std::string title, why;
  nlohmann::json numbers = nlohmann::json::object();
  nlohmann::json renders = nlohmann::json::array();  // [{caption, path, group}]
  std::vector<QuestionOption> options;
  std::string asked_at;
  nlohmann::json to_json() const;
};

struct Answer {
  std::string id, choice;
  std::string picture, label;  // hidden_side_picture: the given picture and its angle (back / left / right)
  std::string who, when;
  std::string source;          // "app" | "cli" | "answers file"
  nlohmann::json to_json() const;
};

// An answer file is accepted only when it answers this question with one of its options (and, for a picture
// option, names an existing picture with a known angle). Otherwise the error says why; it is never guessed.
std::optional<Answer> parse_answer(const nlohmann::json& j, const Question& q, std::string& error);

enum class AskMode {
  Headless,  // write the question, record PENDING_DECISION, continue with the measured result (benchmarks)
  Ask        // write the question and wait for <out>/decisions/<id>.answer.json (bounded by wait_seconds)
};

struct DecisionOptions {
  AskMode mode = AskMode::Headless;
  double wait_seconds = 1800;
  fs::path answers_file;  // answers given beforehand (a JSON array or {"answers": [...]}), matched by question kind
};

// Questions on disk: <out>/decisions/<id>.question.json; answers: <out>/decisions/<id>.answer.json.
class DecisionChannel {
 public:
  DecisionChannel(fs::path out_dir, DecisionOptions opt);
  // Writes the question; returns the answer, or nullopt (headless, timed out, or an invalid answer file:
  // `outcome` says which). `waited` gets the seconds spent waiting for a person (not counted in the time budget).
  // `on_wait` is called about every second while waiting (for a heartbeat in the log).
  std::optional<Answer> ask(Question& q, std::string& outcome, double& waited, const std::function<void(double)>& on_wait = {});
  fs::path dir() const { return dir_; }
  const DecisionOptions& options() const { return opt_; }
  int next_number() { return ++count_; }

 private:
  fs::path out_, dir_;
  DecisionOptions opt_;
  std::vector<nlohmann::json> preset_;  // from answers_file
  std::vector<bool> preset_used_;
  int count_ = 0;
};

// Writes <out>/decisions/<id>.answer.json for `3re-agent --answer` and the app (temp file + rename: the agent
// never reads half a file). Refuses to replace an existing answer.
bool write_answer_file(const fs::path& out_dir, const Answer& a, std::string& error);

// a. The smallest triangle count that passes fidelity, between the budget (known to fail) and the original (passes
// by definition). Ladder 1.5x, 2x, 3x the budget until one passes, then bisection between the last failing and the
// first passing count until they are within max(2% of the budget, 100 triangles), at most `max_probes` probes (default 10).
// `passes(t)` decimates to t and judges fidelity; it may return nullopt when it could not measure (time budget,
// worker failure), which stops the search. Fidelity is not strictly monotonic in t: every probe is returned.
struct NeededSearch {
  int needed = 0;        // smallest passing count found (0 = none measured below the original)
  int highest_fail = 0;  // largest failing count below `needed`
  bool stopped = false;  // the search stopped early (time / measurement), `needed` is an upper bound
  std::vector<std::pair<int, bool>> probes;  // (triangles, passed) in the order tried
};
NeededSearch find_needed_triangles(int budget, int original, const std::function<std::optional<bool>(int)>& passes, int max_probes = 10);

// b. Hidden-side defects of the delivered texture, from the texture worker's report (gate issues), each mapped to
// the kind catalogued in docs/agent/TEXTURE-DIAGNOSIS.md. Only issues a picture of another side can fix count;
// visible-side issues (outline match, picture drift) are listed apart. `inferred` = the hidden side is not seen by
// any picture (TRELLIS's texture or a reconstruction).
struct HiddenSideFinding {
  std::string issue, kind, catalogue;
};
struct HiddenSideAssessment {
  bool inferred = false;
  bool weak = false;  // inferred and at least one hidden-side finding
  std::vector<HiddenSideFinding> findings;
  std::vector<std::string> other_issues;
  nlohmann::json numbers = nlohmann::json::object();
  nlohmann::json to_json() const;
};
HiddenSideAssessment assess_hidden_side(const nlohmann::json& texture_report, bool hybrid);

// A person's answer turns the criterion it decided into ACCEPTED (measured numbers kept in the detail, with who and
// when); nothing else changes. Criteria named in `accepted`: criterion name -> detail suffix.
struct Criterion;
void apply_human_acceptance(std::vector<Criterion>& criteria, const std::vector<std::pair<std::string, std::string>>& accepted);

}  // namespace p3dagent
