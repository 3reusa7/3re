#pragma once
// Chat with the agent (milestone 4, W3): a conversation on the Agent page. Pictures and GLBs come in with "+" or
// drag-and-drop; the brain (rules by default) asks for what is missing and drives the real agent through tools.
// Each message shows who wrote it: you, the brain ("rules" / "model"), or "agent (measured)" for facts read from the
// agent's files. The conversation is a run: Documents\3re\conversations\<time>\run.jsonl.
#include <QWidget>
#include <memory>

#include "chat.hpp"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QThread;
class QTimer;
class QVBoxLayout;

namespace p3dgui {

class AgentChatPanel : public QWidget {
  Q_OBJECT
 public:
  // conversations_dir / runs_dir / agent_exe: where conversations and runs go and the agent to start (tests pass
  // their own; start_agent replaces the process start in tests).
  AgentChatPanel(const QString& conversations_dir, const QString& runs_dir, const QString& agent_exe, QWidget* parent = nullptr,
                 std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> start_agent = {});
  ~AgentChatPanel() override;
  // What a person types and attaches (also used by the demo and tests).
  void send(const QString& text, const QStringList& files = {});
  void attach(const QStringList& files);
  // W5: reopen a conversation folder (after the app was closed): its messages are shown and its run followed again.
  bool openConversation(const QString& dir);
  // The newest conversation folder, when the run it started has not finished ("" = none).
  static QString unfinishedConversation(const QString& conversations_dir);
  QStringList pending() const { return pending_; }
  int messageCount() const;
  const p3dchat::Session* session() const { return session_.get(); }
  QString conversationDir() const { return dir_; }
  // Checks the current run (the timer does it every 2 s) and hands a new question or the result to the brain.
  void poll();
  // A turn (brain + tools) is running on the worker thread.
  bool busy() const { return worker_ != nullptr; }

 signals:
  void turnFinished();
  // The run this chat started has finished (its report is in).
  void runFinished();
  // A run started from the chat: the page shows it in the run viewer.
  void runStarted(const QString& run_dir);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void show(const std::vector<p3dchat::Message>& ms);
  void addBubble(const p3dchat::Message& m);
  void updatePending();
  void runTurn(std::function<void()> work);
  // W4: a view of the finished model where the person circles the problem; sends "<what> mark yaw= x= y= r=".
  void markOn(const QString& view_png, double yaw);

  QString dir_;
  std::unique_ptr<p3dchat::Tools> tools_;
  std::unique_ptr<p3dchat::Provider> brain_;
  std::unique_ptr<p3dchat::Session> session_;
  QScrollArea* scroll_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* list_ = nullptr;
  QLineEdit* input_ = nullptr;
  QPushButton *plus_ = nullptr, *send_ = nullptr;
  QLabel *pending_label_ = nullptr, *brain_label_ = nullptr;
  QStringList pending_;
  QTimer* timer_ = nullptr;
  QThread* worker_ = nullptr;
  QString last_state_;
  int bubbles_ = 0;
};

}  // namespace p3dgui
