#include "agent_decide.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

#include "agent_eval.hpp"
#include "agent_exec.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {

using nlohmann::json;
using pixel3d::to_utf8;

json Question::to_json() const {
  json o = json::array();
  for (const auto& op : options)
    o.push_back({{"id", op.id}, {"label", op.label}, {"detail", op.detail}, {"needs_picture", op.needs_picture}});
  return {{"id", id}, {"kind", kind}, {"title", title}, {"why", why}, {"numbers", numbers}, {"renders", renders},
          {"options", o}, {"asked_at", asked_at}};
}

json Answer::to_json() const {
  json j = {{"id", id}, {"choice", choice}, {"who", who}, {"when", when}, {"source", source}};
  if (!picture.empty()) j["picture"] = picture;
  if (!label.empty()) j["label"] = label;
  return j;
}

namespace {

const std::vector<std::string> kLabels = {"back", "left", "right", "back_left", "back_right", "front_left", "front_right", "auto"};

std::string str_of(const json& j, const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }

std::optional<json> read_json(const fs::path& f) {
  auto bytes = pixel3d::read_file(f, 4u << 20);
  if (!bytes) return std::nullopt;
  try {
    return json::parse(bytes->begin(), bytes->end());
  } catch (...) {
    return std::nullopt;
  }
}

void write_atomic(const fs::path& f, const std::string& text) {
  const auto tmp = fs::path(f).concat(L".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) throw std::runtime_error("cannot write " + to_utf8(tmp));
  }
  if (!MoveFileExW(tmp.wstring().c_str(), f.wstring().c_str(), MOVEFILE_WRITE_THROUGH)) {
    std::error_code ec;
    fs::remove(tmp, ec);
    throw std::runtime_error("cannot create " + to_utf8(f) + " (it exists or the folder is not writable)");
  }
}

}  // namespace

std::optional<Answer> parse_answer(const json& j, const Question& q, std::string& error) {
  if (!j.is_object()) {
    error = "the answer is not a JSON object";
    return std::nullopt;
  }
  Answer a;
  a.id = q.id;
  a.choice = str_of(j, "choice");
  a.who = str_of(j, "who");
  a.when = str_of(j, "when");
  a.source = str_of(j, "source");
  a.picture = str_of(j, "picture");
  a.label = str_of(j, "label");
  if (j.contains("id") && str_of(j, "id") != q.id) {
    error = "the answer is for question " + str_of(j, "id") + ", not " + q.id;
    return std::nullopt;
  }
  const auto op = std::find_if(q.options.begin(), q.options.end(), [&](const QuestionOption& o) { return o.id == a.choice; });
  if (op == q.options.end()) {
    error = "\"" + a.choice + "\" is not one of this question's options";
    return std::nullopt;
  }
  if (a.who.empty()) {
    error = "the answer does not say who decided (\"who\")";
    return std::nullopt;
  }
  if (a.when.empty()) a.when = now_iso();
  if (a.source.empty()) a.source = "answer file";
  if (op->needs_picture) {
    std::error_code ec;
    if (a.picture.empty() || !fs::is_regular_file(pixel3d::path_from_utf8(a.picture), ec)) {
      error = "the picture \"" + a.picture + "\" does not exist";
      return std::nullopt;
    }
    if (a.label.empty()) a.label = "back";
    if (std::find(kLabels.begin(), kLabels.end(), a.label) == kLabels.end()) {
      error = "the picture's angle \"" + a.label + "\" is not one of back, left, right, back_left, back_right, front_left, front_right, auto";
      return std::nullopt;
    }
  } else {
    a.picture.clear();
    a.label.clear();
  }
  return a;
}

DecisionChannel::DecisionChannel(fs::path out_dir, DecisionOptions opt) : out_(std::move(out_dir)), dir_(out_ / "decisions"), opt_(std::move(opt)) {
  if (!opt_.answers_file.empty()) {
    const auto j = read_json(opt_.answers_file);
    if (!j) throw std::runtime_error("the answers file " + to_utf8(opt_.answers_file) + " is missing or not JSON");
    const json list = j->is_array() ? *j : j->value("answers", json::array());
    for (const auto& a : list)
      if (a.is_object()) preset_.push_back(a);
    preset_used_.assign(preset_.size(), false);
  }
}

std::optional<Answer> DecisionChannel::ask(Question& q, std::string& outcome, double& waited, const std::function<void(double)>& on_wait) {
  waited = 0;
  fs::create_directories(dir_);
  if (q.asked_at.empty()) q.asked_at = now_iso();
  write_atomic(dir_ / (q.id + ".question.json"), q.to_json().dump(1));
  // Answers given beforehand (a person decided before the run, e.g. a re-run of a demo): the first unused one for
  // this kind of question. Still a human answer, with that person's name and time.
  for (size_t i = 0; i < preset_.size(); ++i) {
    if (preset_used_[i] || str_of(preset_[i], "kind") != q.kind) continue;
    preset_used_[i] = true;
    json j = preset_[i];
    j.erase("id");
    if (!j.contains("source")) j["source"] = "answers file";
    std::string error;
    auto a = parse_answer(j, q, error);
    if (!a) {
      outcome = "the answers file's answer was refused: " + error;
      return std::nullopt;
    }
    write_atomic(dir_ / (q.id + ".answer.json"), a->to_json().dump(1));
    outcome = "answered (answers file)";
    return a;
  }
  if (opt_.mode == AskMode::Headless) {
    outcome = "PENDING_DECISION (headless run: questions are never answered automatically)";
    return std::nullopt;
  }
  const auto file = dir_ / (q.id + ".answer.json");
  const auto t0 = std::chrono::steady_clock::now();
  double last_beat = 0;
  for (;;) {
    waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::error_code ec;
    if (fs::exists(file, ec)) {
      const auto j = read_json(file);
      std::string error;
      auto a = j ? parse_answer(*j, q, error) : std::nullopt;
      if (!j) error = "the answer file is not JSON";
      if (a) {
        outcome = "answered (" + a->source + ")";
        return a;
      }
      // A wrong answer is kept for the record and the question stays open.
      const auto bad = dir_ / (q.id + ".answer.refused-" + std::to_string(int(waited)) + ".json");
      fs::rename(file, bad, ec);
      outcome = "refused answer: " + error;
      if (on_wait) on_wait(-1);
    }
    if (waited >= opt_.wait_seconds) {
      outcome = "PENDING_DECISION (no answer within " + std::to_string(int(opt_.wait_seconds)) + " s; never answered automatically)";
      return std::nullopt;
    }
    if (on_wait && waited - last_beat >= 15) {
      last_beat = waited;
      on_wait(waited);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
}

bool write_answer_file(const fs::path& out_dir, const Answer& a, std::string& error) {
  try {
    const auto dir = out_dir / "decisions";
    if (!fs::exists(dir / (a.id + ".question.json"))) {
      error = "no question " + a.id + " in " + to_utf8(dir);
      return false;
    }
    if (fs::exists(dir / (a.id + ".answer.json"))) {
      error = "question " + a.id + " is already answered";
      return false;
    }
    write_atomic(dir / (a.id + ".answer.json"), a.to_json().dump(1));
    return true;
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

NeededSearch find_needed_triangles(int budget, int original, const std::function<std::optional<bool>(int)>& passes, int max_probes) {
  NeededSearch s;
  s.highest_fail = budget;
  int lo = budget, hi = 0;
  auto probe = [&](int t) -> std::optional<bool> {
    if (int(s.probes.size()) >= max_probes) return std::nullopt;
    const auto r = passes(t);
    if (r) s.probes.push_back({t, *r});
    return r;
  };
  for (double k : {1.5, 2.0, 3.0}) {
    const int t = int(std::lround(budget * k));
    if (t >= original) break;
    const auto r = probe(t);
    if (!r) {
      s.stopped = true;
      break;
    }
    if (*r) {
      hi = t;
      break;
    }
    lo = t;
  }
  if (hi == 0 && !s.stopped) hi = original;  // the original passes by definition (nothing removed)
  if (hi == 0) {
    s.highest_fail = lo;
    return s;
  }
  const int resolution = std::max(100, int(std::lround(0.02 * budget)));
  while (!s.stopped && hi - lo > resolution) {
    const int mid = lo + (hi - lo) / 2;
    const auto r = probe(mid);
    if (!r) {
      s.stopped = true;
      break;
    }
    (*r ? hi : lo) = mid;
  }
  s.needed = hi;
  s.highest_fail = lo;
  return s;
}

json HiddenSideAssessment::to_json() const {
  json f = json::array();
  for (const auto& x : findings) f.push_back({{"issue", x.issue}, {"kind", x.kind}, {"catalogue", x.catalogue}});
  return {{"inferred", inferred}, {"weak", weak}, {"findings", f}, {"other_issues", other_issues}, {"numbers", numbers}};
}

HiddenSideAssessment assess_hidden_side(const json& rep, bool hybrid) {
  HiddenSideAssessment a;
  // Which surface the pictures do not see: TRELLIS's texture (hybrid) or a reconstruction from the picture's edges.
  json hs = json::object();
  if (rep.contains("candidates") && rep["candidates"].is_array() && !rep["candidates"].empty() && rep["candidates"][0].is_object())
    hs = rep["candidates"][0].value("quality", json::object()).value("hidden_side", json::object());
  const double seen_by_extra = hs.is_object() ? hs.value("back_seen_by_extra_pictures_fraction", 0.0) : 0.0;
  a.inferred = seen_by_extra < 0.5;
  a.numbers = {{"hybrid", hybrid}, {"back_seen_by_extra_pictures_fraction", seen_by_extra}};
  for (const char* k : {"front_copy_correlation", "fragmentation_ratio", "colour_edge_rate", "chroma_ratio", "unsupported_patch_fraction",
                        "luminance_mean", "picture_luminance_mean"})
    if (hs.is_object() && hs.contains(k)) a.numbers[k] = hs[k];
  // The gate's hidden-side issues (auto_texture_quality.cpp, thresholds set on the regression assets) and the kind
  // of defect each is in the B1 catalogue. Matching is on the gate's own wording.
  struct Rule {
    const char* needle;
    const char* kind;
    const char* catalogue;
  };
  static const Rule rules[] = {
      {"repeats the picture's front features", "front copied to the back", "D6 (pixel hood: a face painted on the back of the hood)"},
      {"partly repeat front detail", "front copied to the back", "D6 (pixel hood: a face painted on the back of the hood)"},
      {"large flat colour patches", "flat, low-detail back", "D5, D11, D13, D14 (blurry or mottled backs: pixel hood, rogue, sword, bike)"},
      {"much greyer than the picture", "duller, greyer back", "D1 (the anime-style character: back of the gi duller and browner)"},
      {"appear nowhere in the picture", "foreign patches or smudges on the back", "D10, D15, D16 (vampire cloak blotches, chair stripes, bowl streaks)"},
      {"hidden surfaces are darker", "darker back", "D7, D11, D14 (pixel hood, rogue, bike: dark hidden side)"},
      {"abnormally dark", "darker back", "D7, D11, D14 (pixel hood, rogue, bike: dark hidden side)"},
      {"extrapolated from the picture's visible edges", "no texture information for the hidden sides", "D7 (input limit: front-only picture)"},
  };
  for (const auto& i : rep.value("issues", json::array())) {
    if (!i.is_string()) continue;
    const auto s = i.get<std::string>();
    bool matched = false;
    for (const auto& r : rules)
      if (s.find(r.needle) != std::string::npos) {
        a.findings.push_back({s, r.kind, r.catalogue});
        matched = true;
        break;
      }
    if (!matched) a.other_issues.push_back(s);
  }
  a.weak = a.inferred && !a.findings.empty();
  return a;
}

void apply_human_acceptance(std::vector<Criterion>& criteria, const std::vector<std::pair<std::string, std::string>>& accepted) {
  for (auto& c : criteria)
    for (const auto& [name, why] : accepted)
      if (c.name == name && c.status != "PASS") {
        c.detail += " — " + why;
        c.status = "ACCEPTED";
      }
}

}  // namespace p3dagent
