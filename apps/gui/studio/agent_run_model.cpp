#include "studio/agent_run_model.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cmath>

namespace p3dgui {

using nlohmann::json;

namespace {

QString q(const json& j, const char* k, const QString& fallback = {}) {
  if (!j.is_object() || !j.contains(k)) return fallback;
  const auto& v = j[k];
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  if (v.is_null()) return fallback;
  return QString::fromStdString(v.dump());
}
double num(const json& j, const char* k, double fallback = -1) {
  return j.is_object() && j.contains(k) && j[k].is_number() ? j[k].get<double>() : fallback;
}
QString fixed(double v, int digits) { return QString::number(v, 'f', digits); }
QString tone_of_verdict(const QString& v) {
  if (v == "PASS" || v == "ACCEPTED") return "pass";
  if (v == "REVIEW" || v == "UNVERIFIED" || v == "PENDING_DECISION") return "review";
  if (v == "FAIL" || v == "UNSAFE" || v == "BLOCKED") return "fail";
  return "neutral";
}
QStringList strings(const json& a) {
  QStringList out;
  if (a.is_array())
    for (const auto& s : a)
      if (s.is_string()) out << QString::fromStdString(s.get<std::string>());
  return out;
}
QVector<AgentThumb> thumbs_of(const json& renders) {
  QVector<AgentThumb> t;
  if (renders.is_array())
    for (const auto& r : renders) t.push_back({q(r, "path"), q(r, "caption"), q(r, "group")});
  return t;
}
QString fidelity_line(const json& f) {
  if (!f.is_object()) return {};
  QString s = q(f, "status", "?");
  const auto reasons = strings(f.value("reasons", json::array()));
  if (!reasons.isEmpty()) s += " — " + reasons.join("; ");
  return s;
}

}  // namespace

QString agent_duration(double s) {
  if (s < 0) s = 0;
  if (s < 60) return QString::number(s, 'f', 1) + " s";
  const int total = int(std::lround(s));
  if (total < 3600) return QStringLiteral("%1 m %2 s").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
  return QStringLiteral("%1 h %2 m").arg(total / 3600).arg((total % 3600) / 60, 2, 10, QChar('0'));
}

void AgentRunModel::clear() {
  *this = AgentRunModel();
}

bool AgentRunModel::open(const QString& dir) {
  clear();
  dir_ = QDir(dir).absolutePath();
  return QFileInfo::exists(QDir(dir_).filePath("run.jsonl"));
}

int AgentRunModel::find(const QString& key) const {
  if (key.isEmpty()) return -1;
  for (int i = int(items_.size()) - 1; i >= 0; --i)
    if (items_[i].key == key) return i;
  return -1;
}

int AgentRunModel::add(AgentItem item) {
  items_.push_back(std::move(item));
  changed_.push_back(int(items_.size()) - 1);
  return int(items_.size()) - 1;
}

int AgentRunModel::openQuestion() const {
  for (int i = 0; i < items_.size(); ++i)
    if (items_[i].kind == AgentItem::Kind::Question && items_[i].state == "open") return i;
  return -1;
}

double AgentRunModel::secondsOf(const json& line) {
  const auto t = QDateTime::fromString(q(line, "timestamp"), Qt::ISODateWithMs);
  if (!t.isValid()) return last_.isValid() && started_.isValid() ? started_.msecsTo(last_) / 1000.0 : 0;
  if (!started_.isValid()) started_ = t;
  last_ = t;
  return started_.msecsTo(t) / 1000.0;
}

QVector<int> AgentRunModel::poll() {
  changed_.clear();
  QFile f(QDir(dir_).filePath("run.jsonl"));
  if (!dir_.isEmpty() && f.open(QIODevice::ReadOnly)) {
    if (f.size() > offset_ && f.seek(offset_)) {
      const QByteArray bytes = f.readAll();
      offset_ += bytes.size();
      partial_ += bytes;
      // Only complete lines: the agent flushes one JSON object per line.
      int start = 0;
      for (int nl = partial_.indexOf('\n'); nl >= 0; nl = partial_.indexOf('\n', start)) {
        const QByteArray line = partial_.mid(start, nl - start).trimmed();
        start = nl + 1;
        if (line.isEmpty()) continue;
        json j;
        try {
          j = json::parse(line.constData(), line.constData() + line.size());
        } catch (...) {
          AgentItem bad;
          bad.title = QStringLiteral("Unreadable log line");
          bad.detail = QString::fromUtf8(line.left(200));
          bad.tone = "review";
          add(bad);
          continue;
        }
        try {
          feed(j);
        } catch (const std::exception& e) {
          AgentItem bad;
          bad.title = QStringLiteral("Log line not shown (%1)").arg(q(j, "event", q(j, "step")));
          bad.detail = QString::fromUtf8(e.what());
          bad.tone = "review";
          add(bad);
        }
      }
      partial_ = partial_.mid(start);
    }
  }
  if (finished_ && !report_loaded_) loadReport();
  QVector<int> out;
  for (int i : changed_)
    if (!out.contains(i)) out.push_back(i);
  return out;
}

void AgentRunModel::loadReport() {
  QFile f(QDir(dir_).filePath("report.json"));
  if (!f.open(QIODevice::ReadOnly)) return;
  try {
    const QByteArray b = f.readAll();
    report_ = json::parse(b.constData(), b.constData() + b.size());
  } catch (...) {
    return;  // being written: next poll
  }
  report_loaded_ = true;
  status_ = q(report_, "status", status_);
  // Older runs (milestone 2) logged the candidate tables only in the report.
  for (const char* table : {"decimation_candidates", "texture_candidates"}) {
    const bool dec = std::string(table) == "decimation_candidates";
    const auto& list = report_.value(table, json::array());
    if (list.empty() || find(dec ? "dec" : "tex") >= 0) continue;
    for (const auto& c : list)
      addCandidate(dec, report_.value("elapsed_seconds", 0.0), q(c, "name"),
                   dec ? QStringLiteral("%1 triangles · fidelity %2").arg(q(c, "triangles"), fidelity_line(c.value("fidelity", json::object())))
                       : QStringLiteral("verdict %1 · seen-side agreement dE %2").arg(q(c, "verdict", "?"), fixed(num(c, "agreement_de"), 4)),
                   q(c, "thumbnail"), dec ? q(c, "triangles") + " triangles" : q(c, "name"));
    markCandidates(dec, list);
  }
  AgentItem fin;
  fin.kind = AgentItem::Kind::Final;
  fin.key = "final";
  fin.at = report_.value("elapsed_seconds", 0.0);
  fin.title = QStringLiteral("Result: ") + status_;
  fin.detail = q(report_, "summary");
  fin.tone = tone_of_verdict(status_);
  if (find("final") < 0) add(fin);
}

int AgentRunModel::candidateCard(bool dec, double at) {
  int i = find(dec ? "dec" : "tex");
  if (i >= 0) return i;
  AgentItem it;
  it.kind = AgentItem::Kind::Candidate;
  it.key = dec ? "dec" : "tex";
  it.at = at;
  it.title = dec ? QStringLiteral("Decimation candidates") : QStringLiteral("Texture candidates (TRELLIS texture replays, back views)");
  it.tone = "accent";
  return add(it);
}

void AgentRunModel::addCandidate(bool dec, double at, const QString& name, const QString& line, const QString& thumb, const QString& caption) {
  const int i = candidateCard(dec, at);
  items_[i].lines << line;
  if (!thumb.isEmpty()) items_[i].thumbs.push_back({thumb, caption, {}, name});
  changed_.push_back(i);
}

void AgentRunModel::markCandidates(bool dec, const json& table) {
  const int i = find(dec ? "dec" : "tex");
  if (i < 0) return;
  QString chosen_name;
  for (const auto& c : table) {
    const QString name = q(c, "name");
    const bool chosen = c.value("chosen", false);
    if (chosen) chosen_name = name;
    bool found = false;
    for (auto& t : items_[i].thumbs)
      if (t.id == name) {
        t.badge = chosen ? "chosen" : "rejected";
        found = true;
      }
    if (!found && dec && c.contains("triangles")) {  // added later (a person's decision): a line, no thumbnail
      items_[i].lines << QStringLiteral("%1: %2 triangles · fidelity %3").arg(name, q(c, "triangles"), fidelity_line(c.value("fidelity", json::object())));
    }
  }
  if (!chosen_name.isEmpty()) {
    items_[i].detail = QStringLiteral("Chosen: ") + chosen_name;
    items_[i].tone = "pass";
  } else {
    items_[i].detail = QStringLiteral("None chosen");
    items_[i].tone = "review";
  }
  changed_.push_back(i);
}

QVector<int> AgentRunModel::feed(const json& j) {
  const double at = secondsOf(j);
  const QString ev = q(j, "event");
  AgentItem it;
  it.at = at;
  if (j.contains("step") && ev.isEmpty()) {
    // A finished step (tool, exit code, time, memory).
    running_.clear();
    const QString step = q(j, "step");
    it.kind = AgentItem::Kind::Step;
    if (step == "fidelity") {
      const auto f = j.value("fidelity", json::object());
      const auto jd = f.value("judgement", json::object());
      const auto d = f.value("distance", json::object());
      it.title = QStringLiteral("Fidelity: ") + q(j, "label");
      it.detail = QStringLiteral("%1 · distance p95 %2 %, max %3 % · silhouette IoU min %4 · normals %5°")
                      .arg(q(jd, "status", "?"), fixed(num(d, "p95_pct"), 3), fixed(num(d, "max_pct"), 3),
                           fixed(num(f.value("silhouette_iou", json::object()), "min"), 4), fixed(num(f, "normal_mean_deg"), 1));
      it.lines = strings(jd.value("reasons", json::array()));
      it.tone = tone_of_verdict(q(jd, "status"));
    } else {
      it.title = q(j, "tool", step);
      QString d = QStringLiteral("exit %1 · %2 · peak RAM %3 MiB").arg(q(j, "exit_code"), agent_duration(num(j, "seconds", 0)), q(j, "peak_memory_mib", "0"));
      const double vb = num(j, "vram_before_mib", 0), vp = num(j, "vram_peak_mib", 0);
      if (vp > vb + 16) d += QStringLiteral(" · GPU +%1 MiB").arg(int(vp - vb));
      if (j.value("timed_out", false)) d += QStringLiteral(" · TIMED OUT");
      if (j.value("memory_limit_hit", false)) d += QStringLiteral(" · OUT OF MEMORY (job limit)");
      if (step == "deliver") d = QStringLiteral("%1 · %2").arg(QDir::toNativeSeparators(q(j, "output")), q(j, "method"));
      it.detail = d;
      it.tone = j.value("exit_code", 0) == 0 ? "neutral" : "fail";
    }
    add(it);
  } else if (ev == "start") {
    const auto s = j.value("spec", json::object());
    name_ = q(s, "name");
    it.title = QStringLiteral("Run started: ") + name_;
    it.detail = QStringLiteral("%1 · budget %2 triangles · texture %3 · %4 m")
                    .arg(q(s, "asset_type"), q(s, "triangle_budget"), q(s, "texture_size"), q(s.value("scale", json::object()), "metres"));
    it.tone = "accent";
    add(it);
  } else if (ev == "capabilities") {
    const auto c = j.value("capabilities", json::object());
    it.title = QStringLiteral("Machine and tools checked");
    const auto gpu = c.value("gpu", json::object());
    it.detail = QStringLiteral("RAM free %1 MiB · GPU %2 %3 MiB · disk free %4 MiB")
                    .arg(q(c, "ram_free_mib"), gpu.value("present", false) ? q(gpu, "name") : QStringLiteral("none"), q(gpu, "vram_mib", "0"),
                         q(c, "disk_free_mib"));
    int ok = 0, missing = 0;
    const auto tools = c.value("tools", json::object());
    for (const auto& [k, t] : tools.items()) {
      if (t.value("present", false) && t.value("verified", false)) ++ok;
      else {
        ++missing;
        it.lines << QStringLiteral("%1: %2").arg(QString::fromStdString(k), t.value("present", false) ? q(t, "note") : QStringLiteral("not found"));
      }
    }
    it.lines.prepend(QStringLiteral("%1 tools verified (SHA-256), %2 missing or not verified").arg(ok).arg(missing));
    add(it);
  } else if (ev == "plan") {
    const auto p = j.value("plan", json::object());
    it.kind = AgentItem::Kind::Plan;
    it.title = QStringLiteral("Plan");
    for (const auto& s : p.value("steps", json::array())) it.lines << q(s, "stage") + QStringLiteral(" — ") + q(s, "reason");
    for (const auto& n : strings(p.value("notes", json::array()))) it.lines << QStringLiteral("note: ") + n;
    if (p.value("blocked", false)) {
      it.tone = "fail";
      it.detail = QStringLiteral("BLOCKED: ") + q(p, "blocked_reason");
    }
    add(it);
  } else if (ev == "step_started") {
    running_ = q(j, "tool");
    running_at_ = at;
  } else if (ev == "generated") {
    it.title = QStringLiteral("Generated with TRELLIS.2");
    it.detail = QStringLiteral("%1 · %2").arg(QDir::toNativeSeparators(q(j, "run")), agent_duration(num(j, "total_seconds", 0)));
    add(it);
  } else if (ev == "geometry") {
    it.kind = AgentItem::Kind::Geometry;
    it.title = QStringLiteral("Geometry (%1): %2 triangles").arg(q(j, "label"), q(j, "triangles"));
    const QString v = q(j, "verdict");
    it.detail = QStringLiteral("audit %1 · %2 components · %3 open edges · %4 self-crossings")
                    .arg(v.isEmpty() ? QStringLiteral("not measured") : v, q(j, "components"), q(j, "boundary_edges"), q(j, "transverse_crossings"));
    it.tone = v == "UNSAFE" ? "fail" : "neutral";
    add(it);
  } else if (ev == "decision") {
    const auto d = j.value("decision", json::object());
    it.kind = AgentItem::Kind::Decision;
    const QString action = q(d, "action");
    it.title = QStringLiteral("Decision [%1]: %2").arg(q(d, "rule"), action);
    it.detail = q(d, "reason");
    it.tone = action == "Accept" ? "pass" : action == "HumanReview" ? "review" : action == "Stop" ? "fail" : "accent";
    add(it);
  } else if (ev == "decimation_original") {
    const int i = candidateCard(true, at);
    items_[i].title = QStringLiteral("Decimation candidates: %1 triangles, budget %2").arg(q(j, "triangles"), q(j, "budget"));
    items_[i].thumbs.push_back({q(j, "thumbnail"), QStringLiteral("original, %1 triangles").arg(q(j, "triangles")), {}, "original"});
  } else if (ev == "decimation_candidate") {
    const auto f = j.value("fidelity", json::object());
    QString line = q(j, "name") + ": ";
    line += j.value("ran", false) ? QStringLiteral("%1 triangles (budget %2) · fidelity %3 · geometry %4 · %5 self-crossings")
                                        .arg(q(j, "triangles"), j.value("budget_met", false) ? QStringLiteral("met") : QStringLiteral("not met"),
                                             fidelity_line(f), q(j, "game_geometry", "?"), q(j, "crossings"))
                                  : QStringLiteral("the worker did not produce a mesh");
    addCandidate(true, at, q(j, "name"), line, j.contains("thumbnail") && j["thumbnail"].is_string() ? q(j, "thumbnail") : QString(),
                 QStringLiteral("%1\n%2 triangles · fidelity %3").arg(q(j, "name"), q(j, "triangles"), q(f, "status", "?")));
  } else if (ev == "decimation_candidates" || ev == "texture_candidates") {
    markCandidates(ev == "decimation_candidates", j.value("candidates", json::array()));
  } else if (ev == "fidelity_needs") {
    it.kind = AgentItem::Kind::Search;
    it.title = QStringLiteral("What the shape needs: %1 triangles").arg(q(j, "needs_triangles"));
    for (const auto& p : j.value("probes", json::array()))
      it.lines << QStringLiteral("%1 triangles: %2").arg(q(p, "triangles"), p.value("pass", false) ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
    const auto s = j.value("search", json::object());
    it.detail = q(s, "method");
    if (s.value("stopped_early", false)) it.detail += QStringLiteral(" (stopped early: the number is an upper bound)");
    it.tone = "info";
    add(it);
  } else if (ev == "texture") {
    it.kind = AgentItem::Kind::Texture;
    it.title = QStringLiteral("Texture (%1): %2").arg(q(j, "label"), q(j, "verdict"));
    it.lines = strings(j.value("issues", json::array()));
    it.tone = tone_of_verdict(q(j, "verdict"));
    add(it);
  } else if (ev == "texture_candidate") {
    const auto c = j.value("candidate", json::object());
    QString line = QStringLiteral("%1: verdict %2 · seen-side agreement dE %3").arg(q(c, "name"), q(c, "verdict", "?"), fixed(num(c, "agreement_de"), 4));
    if (c.contains("replay_seconds")) line += QStringLiteral(" · replay %1").arg(agent_duration(num(c, "replay_seconds", 0)));
    addCandidate(false, at, q(c, "name"), line, q(c, "thumbnail"),
                 QStringLiteral("%1\nback · dE %2").arg(q(c, "name"), fixed(num(c, "agreement_de"), 4)));
  } else if (ev == "hidden_side") {
    const auto a = j.value("assessment", json::object());
    it.kind = AgentItem::Kind::HiddenSide;
    const bool weak = a.value("weak", false);
    it.title = weak ? QStringLiteral("Hidden side: weak") : a.value("inferred", false) ? QStringLiteral("Hidden side: inferred, no catalogued defect")
                                                                                     : QStringLiteral("Hidden side: seen by the pictures");
    for (const auto& f : a.value("findings", json::array())) it.lines << q(f, "kind") + QStringLiteral(" — ") + q(f, "catalogue");
    it.tone = weak ? "review" : "pass";
    add(it);
  } else if (ev == "question") {
    const auto qq = j.value("question", json::object());
    mode_ = q(j, "mode");
    it.kind = AgentItem::Kind::Question;
    it.key = q(qq, "id");
    it.question = qq;
    it.title = QStringLiteral("Question %1: %2").arg(q(qq, "id"), q(qq, "title"));
    it.detail = q(qq, "why");
    it.thumbs = thumbs_of(qq.value("renders", json::array()));
    it.state = "open";
    it.tone = "review";
    add(it);
  } else if (ev == "waiting") {
    const int i = find(q(j, "question"));
    if (i >= 0) {
      items_[i].outcome = QStringLiteral("Waiting for an answer: %1").arg(agent_duration(num(j, "waited_seconds", 0)));
      changed_.push_back(i);
    }
  } else if (ev == "answer") {
    const int i = find(q(j, "question"));
    if (i >= 0) {
      const auto a = j.value("answer", json::object());
      items_[i].state = "answered";
      items_[i].tone = "pass";
      items_[i].answer = QStringLiteral("Answered \"%1\" by %2 at %3 (%4)").arg(q(a, "choice"), q(a, "who"), q(a, "when"), q(a, "source"));
      if (a.contains("picture")) items_[i].answer += QStringLiteral(" · picture %1 (%2)").arg(QDir::toNativeSeparators(q(a, "picture")), q(a, "label"));
      items_[i].outcome.clear();
      changed_.push_back(i);
    }
  } else if (ev == "pending_decision") {
    const int i = find(q(j, "question"));
    if (i >= 0) {
      items_[i].state = "pending";
      items_[i].tone = "review";
      items_[i].answer = QStringLiteral("PENDING_DECISION — ") + q(j, "why");
      items_[i].outcome.clear();
      changed_.push_back(i);
    }
  } else if (ev == "decision_applied") {
    const int i = find(q(j, "question"));
    if (i >= 0) {
      items_[i].outcome = q(j, "outcome");
      const auto d = j.value("details", json::object());
      items_[i].before = thumbs_of(d.value("before", json::object()).value("renders", json::array()));
      items_[i].after = thumbs_of(d.value("after", json::object()).value("renders", json::array()));
      changed_.push_back(i);
    }
  } else if (ev == "finished") {
    running_.clear();
    finished_ = true;
    status_ = q(j, "status");
  } else if (!ev.isEmpty()) {
    it.title = ev;
    it.title.replace('_', ' ');
    it.title[0] = it.title[0].toUpper();
    for (const char* k : {"reason", "why", "kept", "error"})
      if (j.contains(k)) it.lines << q(j, k);
    add(it);
  }
  return changed_;
}

}  // namespace p3dgui
