#include "studio/agent_chat_panel.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "pixel3d/common/path_utf8.hpp"
#include "widgets.hpp"

namespace p3dgui {

namespace {
std::filesystem::path fs_path(const QString& s) { return std::filesystem::path(s.toStdWString()); }
QString q(const std::string& utf8) { return QString::fromUtf8(utf8.data(), qsizetype(utf8.size())); }
// A view of the model where a person drags a circle over the problem (pixel coordinates of the view itself).
class MarkCanvas : public QLabel {
 public:
  explicit MarkCanvas(const QPixmap& px, QWidget* parent) : QLabel(parent), px_(px) {
    setFixedSize(px.size());
    setCursor(Qt::CrossCursor);
  }
  bool marked() const { return r_ > 0; }
  QPointF centre() const { return c_; }
  double radius() const { return r_; }

 protected:
  void mousePressEvent(QMouseEvent* e) override { c_ = e->position(); r_ = 4; update(); }
  void mouseMoveEvent(QMouseEvent* e) override {
    r_ = std::max(4.0, std::hypot(e->position().x() - c_.x(), e->position().y() - c_.y()));
    update();
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.drawPixmap(0, 0, px_);
    if (r_ > 0) {
      p.setPen(QPen(QColor(255, 80, 60), 2));
      p.drawEllipse(c_, r_, r_);
    }
  }

 private:
  QPixmap px_;
  QPointF c_;
  double r_ = 0;
};

}  // namespace

AgentChatPanel::AgentChatPanel(const QString& conversations_dir, const QString& runs_dir, const QString& agent_exe, QWidget* parent,
                               std::function<bool(const std::filesystem::path&, const std::filesystem::path&, std::string&)> start_agent)
    : QWidget(parent) {
  setObjectName("agentChat");
  setAcceptDrops(true);
  dir_ = QDir(conversations_dir).filePath(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));
  const QString who = qEnvironmentVariable("USERNAME").isEmpty() ? QStringLiteral("unknown user") : qEnvironmentVariable("USERNAME");
  tools_ = std::make_unique<p3dchat::Tools>(
      p3dchat::ToolEnv{fs_path(dir_), fs_path(runs_dir), fs_path(agent_exe), who.toStdString(),
                       fs_path(QDir(conversations_dir).filePath("../Projects")).lexically_normal(), std::move(start_agent)});
  brain_ = std::make_unique<p3dchat::RulesProvider>();

  auto* col = new QVBoxLayout(this);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(theme::space::sm);
  auto* head = new QHBoxLayout;
  head->addWidget(make_label(tr("CHAT WITH THE AGENT"), "section", this));
  head->addStretch(1);
  brain_label_ = make_chip(tr("Brain: rules · offline"), theme::Tone::Neutral, this);
  brain_label_->setObjectName("agentChatBrain");
  brain_label_->setToolTip(tr("A fixed procedure, no language model: it reads numbers and words (Arabic or English), asks for what is "
                              "missing and starts the agent. Results are always the agent's measurements."));
  head->addWidget(brain_label_);
  col->addLayout(head);

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  content_ = new QWidget;
  content_->setObjectName("agentChatMessages");
  content_->setProperty("surface", "app");
  content_->setAttribute(Qt::WA_StyledBackground);
  list_ = new QVBoxLayout(content_);
  list_->setContentsMargins(0, 0, 8, 8);
  list_->setSpacing(theme::space::sm);
  auto* hello = make_label(tr("Tell me what you need, in Arabic or English, and drop pictures (any order, a turnaround sheet works) or a "
                              "GLB here. Example: \"game-ready character for mobile, 1.8 m tall\". I ask only for what is missing; every "
                              "PASS / REVIEW / FAIL you see comes from the agent's measurements."),
                           "secondary", content_);
  hello->setWordWrap(true);
  list_->addWidget(hello);
  list_->addStretch(1);
  scroll_->setWidget(content_);
  col->addWidget(scroll_, 1);

  pending_label_ = make_label(QString(), "muted", this);
  pending_label_->setObjectName("agentChatPending");
  pending_label_->setWordWrap(true);
  pending_label_->hide();
  col->addWidget(pending_label_);
  auto* row = new QHBoxLayout;
  plus_ = make_button(QStringLiteral("+"), nullptr, {}, this);
  plus_->setObjectName("agentChatAttach");
  plus_->setToolTip(tr("Attach pictures (PNG, JPG, WebP, Aseprite, PSD) or a GLB"));
  plus_->setFixedWidth(36);
  input_ = new QLineEdit(this);
  input_->setObjectName("agentChatInput");
  input_->setPlaceholderText(tr("Message the agent… (Enter to send)"));
  send_ = make_button(tr("Send"), "primary", {}, this);
  send_->setObjectName("agentChatSend");
  row->addWidget(plus_);
  row->addWidget(input_, 1);
  row->addWidget(send_);
  col->addLayout(row);

  connect(plus_, &QPushButton::clicked, this, [this] {
    const auto files = QFileDialog::getOpenFileNames(this, tr("Attach pictures or a GLB"), {},
                                                     tr("Pictures and models (*.png *.jpg *.jpeg *.webp *.bmp *.aseprite *.ase *.psd *.glb)"));
    attach(files);
  });
  auto submit = [this] {
    if (input_->text().trimmed().isEmpty() && pending_.isEmpty()) return;
    send(input_->text().trimmed(), pending_);
    input_->clear();
  };
  connect(send_, &QPushButton::clicked, this, submit);
  connect(input_, &QLineEdit::returnPressed, this, submit);
  // Arabic is written right to left.
  connect(input_, &QLineEdit::textChanged, this, [this](const QString& t) {
    input_->setLayoutDirection(t.isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight);
  });
  timer_ = new QTimer(this);
  timer_->setInterval(2000);
  connect(timer_, &QTimer::timeout, this, &AgentChatPanel::poll);
}

AgentChatPanel::~AgentChatPanel() {
  if (worker_) worker_->wait();  // never leave a turn writing into a destroyed session
}

int AgentChatPanel::messageCount() const { return session_ ? int(session_->messages().size()) : 0; }

void AgentChatPanel::attach(const QStringList& files) {
  for (const auto& f : files)
    if (!f.isEmpty() && !pending_.contains(f)) pending_ << f;
  updatePending();
}

bool AgentChatPanel::openConversation(const QString& dir) {
  if (busy() || !QFileInfo::exists(QDir(dir).filePath("run.jsonl"))) return false;
  try {
    auto env = tools_->env();
    env.conversation_dir = fs_path(dir);
    auto tools = std::make_unique<p3dchat::Tools>(env);
    auto session = std::make_unique<p3dchat::Session>(fs_path(dir), *brain_, *tools);
    session_.reset();
    tools_ = std::move(tools);
    session_ = std::move(session);
    dir_ = dir;
  } catch (const std::exception&) {
    return false;
  }
  show(session_->messages());
  const auto run = session_->current_run();
  if (!run.empty()) {
    const QString r = QString::fromStdWString(run.wstring());
    setProperty("shownRun", r);
    // The state the conversation already spoke about is not repeated.
    last_state_.clear();
    for (auto it = session_->messages().rbegin(); it != session_->messages().rend(); ++it)
      if (it->role == "event" && it->result.is_object()) {
        last_state_ = q(it->result.value("state", std::string()));
        if (it->result.contains("question") && it->result["question"].is_object()) last_state_ += q(it->result["question"].value("id", std::string()));
        break;
      }
    emit runStarted(r);
    timer_->start();
  }
  return true;
}

QString AgentChatPanel::unfinishedConversation(const QString& conversations_dir) {
  const auto dirs = QDir(conversations_dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
  for (const auto& fi : dirs) {
    QFile f(QDir(fi.absoluteFilePath()).filePath("run.jsonl"));
    if (!f.open(QIODevice::ReadOnly)) continue;
    QString run;
    while (!f.atEnd()) {
      const QByteArray line = f.readLine();
      try {
        const auto j = p3dchat::json::parse(line.toStdString());
        if (j.value("event", std::string()) == "tool_result" && j.value("tool", std::string()) == "run_agent" &&
            j.value("result", p3dchat::json::object()).value("ok", false))
          run = q(j["result"].value("run", std::string()));
      } catch (...) {
      }
    }
    if (run.isEmpty()) continue;
    // Only the newest conversation with a run is considered.
    return QFileInfo::exists(QDir(run).filePath("report.json")) ? QString() : fi.absoluteFilePath();
  }
  return {};
}

void AgentChatPanel::updatePending() {
  QStringList names;
  for (const auto& f : pending_) names << QFileInfo(f).fileName();
  pending_label_->setText(tr("Attached: %1").arg(names.join(QStringLiteral(", "))));
  pending_label_->setVisible(!pending_.isEmpty());
}

void AgentChatPanel::send(const QString& text, const QStringList& files) {
  if (busy()) return;  // one turn at a time (the input is disabled meanwhile)
  std::vector<p3dchat::Attachment> att;
  // The files attached with "+" or dropped since the last message go with it.
  QStringList all = pending_;
  for (const auto& f : files)
    if (!all.contains(f)) all << f;
  for (const auto& f : all) att.push_back({fs_path(f), p3dchat::attachment_kind(fs_path(f))});
  pending_.clear();
  updatePending();
  const std::string utf8 = text.toStdString();
  runTurn([this, utf8, att] {
    // The conversation folder is made with the first message (opening the app writes nothing).
    if (!session_) session_ = std::make_unique<p3dchat::Session>(fs_path(dir_), *brain_, *tools_);
    session_->user(utf8, att);
  });
}

// The brain and its tools run off the window's thread (a regional fix takes minutes); the new messages are shown
// when the turn is over. Exceptions end the turn with a message instead of ending the app.
void AgentChatPanel::runTurn(std::function<void()> work) {
  const size_t before = session_ ? session_->messages().size() : 0;
  input_->setEnabled(false);
  send_->setEnabled(false);
  plus_->setEnabled(false);
  brain_label_->setText(tr("Brain: rules · working…"));
  auto error = std::make_shared<QString>();
  worker_ = QThread::create([work, error] {
    try {
      work();
    } catch (const std::exception& e) {
      *error = QString::fromUtf8(e.what());
    } catch (...) {
      *error = QStringLiteral("unknown error");
    }
  });
  connect(worker_, &QThread::finished, this, [this, before, error] {
    worker_->deleteLater();
    worker_ = nullptr;
    input_->setEnabled(true);
    send_->setEnabled(true);
    plus_->setEnabled(true);
    brain_label_->setText(tr("Brain: rules · offline"));
    if (session_) show({session_->messages().begin() + long(std::min(before, session_->messages().size())), session_->messages().end()});
    if (!error->isEmpty()) {
      p3dchat::Message m;
      m.role = "assistant";
      m.author = "rules";
      m.text = tr("Something failed on my side: %1").arg(*error).toStdString();
      addBubble(m);
    }
    const auto run = session_ ? session_->current_run() : std::filesystem::path();
    if (!run.empty()) {
      const QString r = QString::fromStdWString(run.wstring());
      if (!timer_->isActive()) timer_->start();
      if (property("shownRun").toString() != r) {
        setProperty("shownRun", r);
        last_state_.clear();
        emit runStarted(r);
      }
    }
    emit turnFinished();
  });
  worker_->start();
}

void AgentChatPanel::poll() {
  if (!session_ || busy()) return;
  const auto run = session_->current_run();
  if (run.empty()) return;
  p3dchat::json state;
  try {
    state = p3dchat::read_run_state(run);
  } catch (const std::exception&) {
    return;  // a half-written file: read again at the next tick
  }
  QString key = q(state.value("state", std::string()));
  if (state.contains("question") && state["question"].is_object()) key += q(state["question"].value("id", std::string()));
  if (key == last_state_) return;
  last_state_ = key;
  const std::string st = state.value("state", std::string());
  if (st != "waiting" && st != "finished") return;  // the run viewer shows progress; the chat speaks at decisions and the end
  if (st == "finished") {
    timer_->stop();
    emit runFinished();
  }
  runTurn([this, state] { session_->event(state); });
}

void AgentChatPanel::show(const std::vector<p3dchat::Message>& ms) {
  for (const auto& m : ms) addBubble(m);
  QCoreApplication::processEvents();
  scroll_->verticalScrollBar()->setValue(scroll_->verticalScrollBar()->maximum());
  // Once the new bubbles are laid out, the range grows: scroll again.
  QTimer::singleShot(0, this, [this] { scroll_->verticalScrollBar()->setValue(scroll_->verticalScrollBar()->maximum()); });
}

void AgentChatPanel::addBubble(const p3dchat::Message& m) {
  if (m.role == "assistant" && m.text.empty() && m.calls.empty()) return;
  auto* card = new Card(content_);
  card->setObjectName(QStringLiteral("agentChatMessage%1").arg(bubbles_++));
  card->setProperty("role", q(m.role));
  auto* lay = new QVBoxLayout(card);
  lay->setContentsMargins(12, 8, 12, 8);
  lay->setSpacing(2);
  QString who;
  theme::Tone tone = theme::Tone::Neutral;
  if (m.role == "user") who = tr("You"), tone = theme::Tone::Accent;
  else if (m.role == "assistant") who = m.author == "rules" ? tr("Agent · rules (explanation)") : tr("Agent · %1 (explanation)").arg(q(m.author));
  else who = tr("Agent · measured"), tone = theme::Tone::Info;
  card->setTone(tone);
  lay->addWidget(make_label(who, "muted", card));
  QString body = q(m.text);
  if (m.role == "tool") {
    // Tool results: a one-line fact, not the raw JSON.
    const auto& r = m.result;
    body = q(m.tool) + QStringLiteral(": ") + (r.value("ok", false) ? tr("ok") : r.value("waiting", false) ? tr("waiting") : tr("refused"));
    if (r.value("waiting", false)) body += QStringLiteral(" · ") + q(r.value("reason", std::string()));
    if (r.contains("spec")) body += QStringLiteral(" · ") + QFileInfo(q(r["spec"].get<std::string>())).fileName();
    if (r.contains("run") && m.tool == "run_agent") body += QStringLiteral(" · ") + q(r["run"].get<std::string>());
    if (r.contains("error")) body += QStringLiteral(" · ") + q(r["error"].get<std::string>());
    if (r.contains("errors") && !r["errors"].empty()) body += QStringLiteral(" · ") + q(r["errors"].dump());
  } else if (m.role == "event") {
    body = tr("Run: %1").arg(q(m.result.value("state", std::string())));
  }
  for (const auto& c : m.calls) body += (body.isEmpty() ? QString() : QStringLiteral("\n")) + tr("→ calls %1").arg(q(c.name));
  if (!m.attachments.empty()) {
    QStringList names;
    for (const auto& a : m.attachments) names << QString::fromStdWString(a.path.filename().wstring());
    body += (body.isEmpty() ? QString() : QStringLiteral("\n")) + tr("Attached: %1").arg(names.join(QStringLiteral(", ")));
  }
  auto* text = make_label(body, m.role == "tool" || m.role == "event" ? "small" : nullptr, card);
  text->setWordWrap(true);
  text->setTextInteractionFlags(Qt::TextSelectableByMouse);
  if (q(m.text).isRightToLeft()) {  // the first strong character decides (an English sentence may quote an Arabic name)
    text->setLayoutDirection(Qt::RightToLeft);
    text->setAlignment(Qt::AlignRight);
  }
  lay->addWidget(text);
  // W4: the views to mark a problem on (click one), and the regional fix's close-ups (before / attempts).
  if (m.role == "tool" && m.tool == "region_views" && m.result.value("ok", false)) {
    auto* row = new QHBoxLayout;
    for (const auto& v : m.result.value("views", p3dchat::json::array())) {
      const QString file = q(v.value("png", std::string()));
      const double yaw = v.value("yaw", 0.0);
      auto* b = new QPushButton(card);
      b->setObjectName(QStringLiteral("agentChatView%1").arg(int(yaw)));
      b->setIcon(QIcon(QPixmap(file).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
      b->setIconSize(QSize(64, 64));
      b->setToolTip(tr("Mark the problem on this view (yaw %1°)").arg(yaw));
      connect(b, &QPushButton::clicked, this, [this, file, yaw] { markOn(file, yaw); });
      row->addWidget(b);
    }
    row->addStretch(1);
    lay->addLayout(row);
  }
  if (m.role == "tool" && m.tool == "regional_fix" && m.result.value("ok", false)) {
    auto* row = new QHBoxLayout;
    auto add = [&](const QString& file, const QString& cap) {
      QPixmap px(file);
      if (px.isNull()) return;
      auto* cell = new QVBoxLayout;
      auto* l = new QLabel(card);
      l->setPixmap(px.scaled(150, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      cell->addWidget(l);
      cell->addWidget(make_label(cap, "small", card));
      row->addLayout(cell);
    };
    const QString dir = q(m.result.value("dir", std::string()));
    add(QDir(dir).filePath("before-closeup.png"), tr("before"));
    int k = 0;
    for (const auto& a : m.result.value("attempts", p3dchat::json::array())) {
      ++k;
      if (a.value("status", std::string()) == "made") add(q(a.value("closeup", std::string())), tr("attempt %1").arg(k));
    }
    row->addStretch(1);
    lay->addLayout(row);
  }
  // Thumbnails of attached pictures.
  auto* thumbs = new QHBoxLayout;
  bool any = false;
  for (const auto& a : m.attachments) {
    if (a.kind != "picture") continue;
    QPixmap px(QString::fromStdWString(a.path.wstring()));
    if (px.isNull()) continue;
    auto* l = new QLabel(card);
    l->setPixmap(px.scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    thumbs->addWidget(l);
    any = true;
  }
  thumbs->addStretch(1);
  if (any) lay->addLayout(thumbs);
  else delete thumbs;
  list_->insertWidget(list_->count() - 1, card);
}

void AgentChatPanel::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void AgentChatPanel::dropEvent(QDropEvent* e) {
  QStringList files;
  for (const auto& u : e->mimeData()->urls())
    if (u.isLocalFile()) files << u.toLocalFile();
  attach(files);
  e->acceptProposedAction();
}

}  // namespace p3dgui

namespace p3dgui {

void AgentChatPanel::markOn(const QString& view_png, double yaw) {
  QPixmap px(view_png);
  if (px.isNull()) return;
  QDialog dlg(this);
  dlg.setWindowTitle(tr("Mark the problem (yaw %1°)").arg(yaw));
  auto* col = new QVBoxLayout(&dlg);
  col->addWidget(make_label(tr("Press on the problem and drag to size the circle."), "secondary", &dlg));
  auto* canvas = new MarkCanvas(px, &dlg);
  col->addWidget(canvas);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  col->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  if (dlg.exec() != QDialog::Accepted || !canvas->marked()) return;
  bool ok = false;
  const QString what = QInputDialog::getText(this, tr("What is wrong here?"), tr("For example: the eye is smeared / this part of the cloak is the wrong colour"),
                                             QLineEdit::Normal, QString(), &ok);
  if (!ok) return;
  send((what.isEmpty() ? tr("There is a problem here.") : what) +
       QStringLiteral(" mark yaw=%1 x=%2 y=%3 r=%4")
           .arg(yaw)
           .arg(int(std::lround(canvas->centre().x())))
           .arg(int(std::lround(canvas->centre().y())))
           .arg(int(std::lround(canvas->radius()))));
}

}  // namespace p3dgui
