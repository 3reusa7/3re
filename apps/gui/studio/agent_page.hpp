#pragma once
// Agent workspace: the run viewer (docs/agent/PLAN-M3.md, part 2). A timeline of one 3re-agent run — every stage,
// tool, decision with its reason and numbers, rejected candidates with thumbnails, questions with answer buttons and
// the final deliverable with its Godot result. It reads the run's run.jsonl / report.json (live while the agent
// runs, polled every 500 ms); it starts the agent itself with --ask so a person can answer here.
#include <QPointer>
#include <QProcess>
#include <QWidget>

#include "studio/agent_run_model.hpp"

class QLabel;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace p3dgui {

class AgentChatPanel;

class AgentPage : public QWidget {
  Q_OBJECT
 public:
  explicit AgentPage(QWidget* parent = nullptr);
  ~AgentPage() override;
  // Shows this run folder (finished or running).
  void openRun(const QString& dir);
  // Starts 3re-agent.exe on a spec with --ask; the run folder is out_dir (created by the agent, must not exist).
  bool startRun(const QString& spec, const QString& out_dir, QString* error = nullptr);
  const AgentRunModel& model() const { return model_; }
  // Writes the answer file for question item `index` (the agent picks it up). Returns false with a reason.
  bool answer(int index, const QString& choice, const QString& picture = {}, const QString& label = {}, QString* error = nullptr);
  // Polls the log now (the timer does it every 500 ms).
  void refresh();
  // For screenshots / the demo: scroll to "question", "final", "top" or an item index.
  void scrollTo(const QString& where);
  QWidget* timeline() const { return content_; }
  AgentChatPanel* chat() const { return chat_; }
  bool agentRunning() const { return proc_ && proc_->state() != QProcess::NotRunning; }

 signals:
  void busyChanged(bool busy, const QString& text);

 private:
  QWidget* buildItem(int index);
  QWidget* buildQuestion(int index, QWidget* card);
  QWidget* buildFinal(QWidget* card);
  QWidget* thumbRow(const QVector<AgentThumb>& thumbs, QWidget* parent, int px);
  void rebuild(int index);
  void updateHeader();
  void chooseRun();
  void chooseSpec();

  AgentRunModel model_;
  QLabel *path_ = nullptr, *status_ = nullptr, *live_ = nullptr, *empty_ = nullptr;
  QPushButton *open_ = nullptr, *run_ = nullptr, *folder_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* list_ = nullptr;
  QVector<QWidget*> cards_;
  QTimer* timer_ = nullptr;
  QPointer<QProcess> proc_;
  AgentChatPanel* chat_ = nullptr;
};

}  // namespace p3dgui
