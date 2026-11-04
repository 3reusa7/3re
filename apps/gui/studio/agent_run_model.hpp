#pragma once
// The agent run viewer's data (docs/agent/PLAN-M3.md, part 2): a timeline built from the agent's own run.jsonl
// (read incrementally while the agent runs) and report.json once it has finished. Nothing is invented: every item is
// one or more log lines, with their numbers.
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>
#include <nlohmann/json.hpp>

namespace p3dgui {

struct AgentThumb {
  QString path, caption, badge;  // badge: "chosen", "rejected", "" (questions: the render group)
  QString id;                    // candidates: the candidate's name
};

struct AgentItem {
  enum class Kind { Info, Plan, Step, Geometry, Decision, Candidate, Search, Texture, HiddenSide, Question, Final };
  Kind kind = Kind::Info;
  QString key;    // stable id for updates ("q1", "dec:guarded", "tex:seed 1", ...)
  double at = 0;  // seconds since the run started (log timestamps)
  QString title, detail;
  QStringList lines;
  QString tone = "neutral";  // neutral, accent, pass, review, fail, info
  QVector<AgentThumb> thumbs;
  // Questions (kind Question).
  nlohmann::json question;           // as the agent wrote it
  QString state;                     // open, answered, pending
  QString answer, outcome;
  QVector<AgentThumb> before, after; // decision applied: before / after renders
};

class AgentRunModel {
 public:
  // A run folder (contains run.jsonl). Returns false when there is no log yet.
  bool open(const QString& dir);
  void clear();
  // Reads what was appended since the last call; returns the indices of items added or changed.
  QVector<int> poll();
  const QVector<AgentItem>& items() const { return items_; }
  QString dir() const { return dir_; }
  bool finished() const { return finished_; }
  bool reportLoaded() const { return report_loaded_; }
  const nlohmann::json& report() const { return report_; }
  QString status() const { return status_; }
  QString name() const { return name_; }
  QString mode() const { return mode_; }  // "ask" or "headless" (from the questions), "" before any question
  // What the agent runs now (a step started and not yet logged as done), "" when idle or finished.
  QString runningTool() const { return running_; }
  double runningSince() const { return running_at_; }
  QDateTime started() const { return started_; }
  QDateTime lastEvent() const { return last_; }
  int openQuestion() const;  // index of a question waiting for an answer, -1 = none

  // For tests: feed one log line.
  QVector<int> feed(const nlohmann::json& line);

 private:
  int add(AgentItem item);
  int find(const QString& key) const;
  double secondsOf(const nlohmann::json& line);
  void loadReport();
  // Candidates of one kind share one card ("dec" / "tex"), side by side, each with its thumbnail and numbers.
  int candidateCard(bool dec, double at);
  void addCandidate(bool dec, double at, const QString& name, const QString& line, const QString& thumb, const QString& caption);
  void markCandidates(bool dec, const nlohmann::json& table);

  QString dir_, status_, name_, mode_, running_;
  qint64 offset_ = 0;
  QByteArray partial_;
  QVector<AgentItem> items_;
  QDateTime started_, last_;
  double running_at_ = 0;
  bool finished_ = false, report_loaded_ = false;
  nlohmann::json report_;
  QVector<int> changed_;
};

// "1 m 05 s" style.
QString agent_duration(double seconds);

}  // namespace p3dgui
