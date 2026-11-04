#include "studio/agent_page.hpp"

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "agent/agent_spec.hpp"
#include "chat.hpp"
#include "studio/agent_chat_panel.hpp"
#include "widgets.hpp"

namespace p3dgui {

using nlohmann::json;

namespace {

theme::Tone tone_from(const QString& t) {
  if (t == "pass") return theme::Tone::Pass;
  if (t == "review") return theme::Tone::Review;
  if (t == "fail") return theme::Tone::Fail;
  if (t == "info") return theme::Tone::Info;
  if (t == "accent") return theme::Tone::Accent;
  return theme::Tone::Neutral;
}
theme::Tone tone_of_status(const QString& s) {
  if (s == "PASS" || s == "ACCEPTED") return theme::Tone::Pass;
  if (s == "FAIL" || s == "BLOCKED" || s == "UNSAFE") return theme::Tone::Fail;
  if (s == "REVIEW" || s == "UNVERIFIED" || s == "PENDING_DECISION") return theme::Tone::Review;
  return theme::Tone::Neutral;
}
QString qj(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || j[k].is_null()) return {};
  return j[k].is_string() ? QString::fromStdString(j[k].get<std::string>()) : QString::fromStdString(j[k].dump());
}
QLabel* wrapped(const QString& text, const char* type, QWidget* parent) {
  auto* l = make_label(text, type, parent);
  l->setWordWrap(true);
  l->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return l;
}
QString time_label(double s) {
  const int t = int(s);
  return QStringLiteral("%1:%2").arg(t / 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
}

}  // namespace

namespace {
// Documents\3re (conversations, agent runs); THREERE_DOCUMENTS replaces Documents (tests: never the person's folder).
QString documents_3re() {
  const QString o = qEnvironmentVariable("THREERE_DOCUMENTS");
  return (o.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : o) + "/3re";
}
}  // namespace

AgentPage::AgentPage(QWidget* parent) : QWidget(parent) {
  // Milestone 4: the chat on the left, the run viewer on the right.
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(24, 20, 0, 0);
  outer->setSpacing(theme::space::lg);
  const QString docs = documents_3re();
  chat_ = new AgentChatPanel(docs + "/conversations", docs + "/agent-runs", QDir(QCoreApplication::applicationDirPath()).filePath("3re-agent.exe"), this);
  chat_->setMinimumWidth(380);
  chat_->setMaximumWidth(520);
  outer->addWidget(chat_, 2);
  auto* viewer = new QWidget(this);
  outer->addWidget(viewer, 3);
  connect(chat_, &AgentChatPanel::runStarted, this, [this](const QString& dir) {
    openRun(dir);
    emit busyChanged(true, tr("Agent running"));
  });
  connect(chat_, &AgentChatPanel::runFinished, this, [this] { emit busyChanged(false, {}); });
  // W5: an agent run survives closing the app; at the next start its conversation and run viewer reattach.
  QTimer::singleShot(0, this, [this, docs] {
    const QString unfinished = AgentChatPanel::unfinishedConversation(docs + "/conversations");
    if (!unfinished.isEmpty()) chat_->openConversation(unfinished);
  });
  auto* col = new QVBoxLayout(viewer);
  col->setContentsMargins(0, 0, 32, 0);
  col->setSpacing(theme::space::md);

  auto* bar = new QHBoxLayout;
  bar->setSpacing(theme::space::sm);
  open_ = make_button(tr("Open run…"), nullptr, icon(Icon::Open), this);
  open_->setObjectName("agentOpenRun");
  open_->setToolTip(tr("Show a 3re-agent run folder (run.jsonl, report.json)"));
  run_ = make_button(tr("Run a spec…"), "primary", icon(Icon::Reconstruct, IconStyle::OnAccent), this);
  run_->setObjectName("agentRunSpec");
  run_->setToolTip(tr("Start 3re-agent on an asset spec; it asks you here when it cannot decide alone"));
  folder_ = make_button(tr("Open folder"), "ghost", icon(Icon::Folder), this);
  folder_->setEnabled(false);
  status_ = make_chip(tr("No run"), theme::Tone::Neutral, this);
  status_->setObjectName("agentStatus");
  live_ = make_label(QString(), "secondary", this);
  live_->setObjectName("agentLive");
  bar->addWidget(open_);
  bar->addWidget(run_);
  bar->addWidget(folder_);
  bar->addSpacing(theme::space::md);
  bar->addWidget(status_);
  bar->addWidget(live_, 1);
  col->addLayout(bar);
  path_ = make_label(QString(), "muted", this);
  path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  col->addWidget(path_);

  scroll_ = new QScrollArea(this);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  content_ = new QWidget;
  content_->setObjectName("agentTimeline");
  content_->setProperty("surface", "app");
  content_->setAttribute(Qt::WA_StyledBackground);
  list_ = new QVBoxLayout(content_);
  list_->setContentsMargins(0, 0, 16, 24);
  list_->setSpacing(theme::space::sm);
  empty_ = wrapped(tr("Open a run folder written by 3re-agent, or run a spec. The timeline shows every stage, tool, decision and "
                      "candidate from the agent's own log, live while it runs. When the agent cannot decide alone it asks here."),
                   "secondary", content_);
  list_->addWidget(empty_);
  list_->addStretch(1);
  scroll_->setWidget(content_);
  col->addWidget(scroll_, 1);

  timer_ = new QTimer(this);
  timer_->setInterval(500);
  connect(timer_, &QTimer::timeout, this, &AgentPage::refresh);
  connect(open_, &QPushButton::clicked, this, &AgentPage::chooseRun);
  connect(run_, &QPushButton::clicked, this, &AgentPage::chooseSpec);
  connect(folder_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(model_.dir())); });
}

AgentPage::~AgentPage() {
  // Never leave an agent waiting for this window: it would wait until its own timeout.
  if (proc_ && proc_->state() != QProcess::NotRunning) proc_->kill();
}

void AgentPage::chooseRun() {
  const QString dir = QFileDialog::getExistingDirectory(this, tr("Open an agent run"), model_.dir());
  if (!dir.isEmpty()) openRun(dir);
}

void AgentPage::chooseSpec() {
  const QString spec = QFileDialog::getOpenFileName(this, tr("Asset spec for 3re-agent"), QString(), tr("Asset spec (*.json)"));
  if (spec.isEmpty()) return;
  const QString base = documents_3re() + "/agent-runs";
  const QString out = QDir(base).filePath(QFileInfo(spec).completeBaseName() + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
  QString error;
  if (!startRun(spec, out, &error)) {
    live_->setText(error);
    set_chip(status_, tr("Not started"), theme::Tone::Fail);
  }
}

bool AgentPage::startRun(const QString& spec, const QString& out_dir, QString* error) {
  if (agentRunning()) {
    if (error) *error = tr("An agent run is already running from this window.");
    return false;
  }
  const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("3re-agent.exe");
  if (!QFileInfo::exists(exe)) {
    if (error) *error = tr("3re-agent.exe is not next to the app (%1).").arg(QDir::toNativeSeparators(exe));
    return false;
  }
  // W5: the same admission as the chat (one agent run at a time, free RAM measured now).
  {
    const auto r = p3dagent::load_spec(std::filesystem::path(spec.toStdWString()));
    const bool generates = r.spec && r.spec->kind == p3dagent::InputKind::Picture;
    const auto adm = p3dchat::admit_run(generates, p3dchat::free_ram_mib(), p3dchat::count_agent_processes());
    if (!adm.start) {
      if (error) *error = tr("Waiting — not started: %1").arg(QString::fromStdString(adm.reason));
      return false;
    }
  }
  if (QFileInfo::exists(out_dir) && !QDir(out_dir).isEmpty()) {
    if (error) *error = tr("The run folder exists and is not empty: %1").arg(QDir::toNativeSeparators(out_dir));
    return false;
  }
  QDir().mkpath(QFileInfo(out_dir).absolutePath());
  proc_ = new QProcess(this);
  proc_->setProgram(exe);
  proc_->setArguments({spec, "--out", out_dir, "--ask", "--wait-seconds", "3600"});
  proc_->setStandardOutputFile(out_dir + ".console.txt");
  proc_->setStandardErrorFile(out_dir + ".console.txt", QIODevice::Append);
  connect(proc_, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
    refresh();
    emit busyChanged(false, {});
  });
  proc_->start();
  if (!proc_->waitForStarted(5000)) {
    if (error) *error = tr("3re-agent did not start: %1").arg(proc_->errorString());
    return false;
  }
  emit busyChanged(true, tr("Agent running"));
  openRun(out_dir);
  return true;
}

void AgentPage::openRun(const QString& dir) {
  for (auto* c : cards_) c->deleteLater();
  cards_.clear();
  model_.open(dir);
  empty_->setVisible(false);
  path_->setText(QDir::toNativeSeparators(model_.dir()));
  folder_->setEnabled(true);
  refresh();
  timer_->start();
}

void AgentPage::refresh() {
  const auto changed = model_.poll();
  for (int i : changed) rebuild(i);
  updateHeader();
  // A finished run whose report is in needs no more polling.
  if (model_.finished() && model_.reportLoaded() && !agentRunning()) timer_->stop();
}

void AgentPage::updateHeader() {
  if (model_.dir().isEmpty()) return;
  QString status = model_.status();
  const int open = model_.openQuestion();
  if (status.isEmpty()) status = open >= 0 ? tr("WAITING FOR YOU") : model_.items().isEmpty() ? tr("Starting") : tr("Running");
  set_chip(status_, status, open >= 0 && !model_.finished() ? theme::Tone::Review : model_.finished() ? tone_of_status(status) : theme::Tone::Accent);
  QString live;
  if (model_.started().isValid()) {
    const double elapsed = model_.finished() ? model_.started().msecsTo(model_.lastEvent()) / 1000.0
                                             : model_.started().msecsTo(QDateTime::currentDateTime()) / 1000.0;
    live = (model_.name().isEmpty() ? QString() : model_.name() + " · ") + agent_duration(elapsed);
  }
  if (!model_.finished() && !model_.runningTool().isEmpty()) live += tr(" · running: %1").arg(model_.runningTool());
  if (!model_.finished() && open >= 0) live += tr(" · question %1 waits for an answer").arg(model_.items()[open].key);
  if (!model_.finished() && !agentRunning() && model_.lastEvent().isValid() && model_.lastEvent().secsTo(QDateTime::currentDateTime()) > 120)
    live += tr(" · no new log line for %1").arg(agent_duration(model_.lastEvent().secsTo(QDateTime::currentDateTime())));
  live_->setText(live);
}

void AgentPage::rebuild(int index) {
  QWidget* w = buildItem(index);
  if (index < cards_.size()) {
    const int pos = list_->indexOf(cards_[index]);
    list_->insertWidget(pos, w);
    cards_[index]->deleteLater();
    cards_[index] = w;
  } else {
    while (cards_.size() < index) cards_.push_back(nullptr);  // not expected: items arrive in order
    list_->insertWidget(list_->count() - 1, w);
    cards_.push_back(w);
  }
}

QWidget* AgentPage::thumbRow(const QVector<AgentThumb>& thumbs, QWidget* parent, int px) {
  auto* host = new QWidget(parent);
  auto* row = new QHBoxLayout(host);
  row->setContentsMargins(0, 4, 0, 0);
  row->setSpacing(theme::space::sm);
  for (const auto& t : thumbs) {
    auto* cell = new QVBoxLayout;
    auto* pic = new QLabel(host);
    QImageReader reader(t.path);
    const QImage img = reader.read();
    if (!img.isNull()) pic->setPixmap(QPixmap::fromImage(img.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    else pic->setText(tr("(no image)"));
    pic->setToolTip(QDir::toNativeSeparators(t.path));
    cell->addWidget(pic);
    auto* cap = make_label(t.caption, "small", host);
    cap->setMaximumWidth(px);
    cap->setWordWrap(true);
    cell->addWidget(cap);
    if (t.badge == "chosen" || t.badge == "rejected")
      cell->addWidget(make_chip(t.badge == "chosen" ? tr("Chosen") : tr("Rejected"), t.badge == "chosen" ? theme::Tone::Pass : theme::Tone::Neutral, host), 0,
                      Qt::AlignLeft);
    row->addLayout(cell);
  }
  row->addStretch(1);
  return host;
}

QWidget* AgentPage::buildItem(int index) {
  const AgentItem& it = model_.items()[index];
  auto* card = new Card(content_);
  card->setObjectName(QStringLiteral("agentItem%1").arg(index));
  card->setProperty("agentKey", it.key);
  card->setTone(tone_from(it.tone));
  auto* lay = new QVBoxLayout(card);
  lay->setContentsMargins(16, 10, 16, 10);
  lay->setSpacing(4);
  auto* head = new QHBoxLayout;
  head->addWidget(make_label(time_label(it.at), "muted", card));
  auto* title = wrapped(it.title, it.kind == AgentItem::Kind::Question || it.kind == AgentItem::Kind::Final ? "title" : nullptr, card);
  head->addWidget(title, 1);
  lay->addLayout(head);
  if (!it.detail.isEmpty()) lay->addWidget(wrapped(it.detail, "secondary", card));
  for (const auto& l : it.lines) lay->addWidget(wrapped(QStringLiteral("• ") + l, "small", card));
  if (it.kind == AgentItem::Kind::Question) return buildQuestion(index, card);
  if (it.kind == AgentItem::Kind::Final) return buildFinal(card);
  if (!it.thumbs.isEmpty()) lay->addWidget(thumbRow(it.thumbs, card, 128));
  return card;
}

QWidget* AgentPage::buildQuestion(int index, QWidget* card) {
  const AgentItem& it = model_.items()[index];
  auto* lay = static_cast<QVBoxLayout*>(card->layout());
  const auto& qq = it.question;
  const auto& n = qq.value("numbers", json::object());
  // The numbers a person decides on.
  if (qj(qq, "kind") == "budget_or_fidelity") {
    const auto at_budget = n.value("at_budget", json::object());
    const auto at_needed = n.value("at_needed", json::object());
    auto row = [&](const QString& name, const json& c) {
      const auto d = c.value("distance", json::object());
      const auto f = c.value("fidelity", json::object());
      return QStringLiteral("%1: %2 triangles · fidelity %3 · distance p95 %4 % · silhouette IoU min %5")
          .arg(name, qj(c, "triangles"), qj(f, "status"), QString::number(d.value("p95_pct", -1.0), 'f', 3),
               QString::number(c.value("silhouette_iou_min", -1.0), 'f', 4));
    };
    lay->addWidget(wrapped(tr("Budget %1 · original %2 triangles").arg(qj(n, "budget"), qj(n, "original_triangles")), "small", card));
    if (!at_budget.empty()) lay->addWidget(wrapped(row(tr("At the budget"), at_budget), "small", card));
    if (!at_needed.empty()) lay->addWidget(wrapped(row(tr("At the needed count"), at_needed), "small", card));
  } else {
    for (const auto& i : n.value("issues", json::array()))
      if (i.is_string()) lay->addWidget(wrapped(QStringLiteral("• ") + QString::fromStdString(i.get<std::string>()), "small", card));
  }
  // Renders grouped (original / budget / needed, or before).
  QMap<QString, QVector<AgentThumb>> groups;
  QStringList order;
  for (const auto& t : it.thumbs) {
    if (!groups.contains(t.badge)) order << t.badge;
    groups[t.badge].push_back({t.path, t.caption, {}});
  }
  // Once a picture was applied, before / after replace the question's own renders (they are the "before").
  const bool compared = !it.before.isEmpty() || !it.after.isEmpty();
  for (const auto& g : compared ? QStringList() : order) {
    lay->addWidget(make_label(groups[g].front().caption, "section", card));
    auto thumbs = groups[g];
    for (auto& t : thumbs) t.caption.clear();
    lay->addWidget(thumbRow(thumbs, card, 224));
  }
  // Answer buttons while the agent waits; the answer otherwise.
  if (it.state == "open" && model_.mode() == "ask" && !model_.finished()) {
    auto* buttons = new QHBoxLayout;
    bool first = true;
    for (const auto& o : qq.value("options", json::array())) {
      const QString id = qj(o, "id");
      auto* b = make_button(qj(o, "label"), first ? "primary" : nullptr, {}, card);
      b->setObjectName(QStringLiteral("agentAnswer_%1_%2").arg(it.key, id));
      b->setToolTip(qj(o, "detail"));
      first = false;
      if (o.value("needs_picture", false)) {
        auto* label = new QComboBox(card);
        label->setObjectName(QStringLiteral("agentPictureLabel_%1").arg(it.key));
        label->addItems({"back", "left", "right", "back_left", "back_right"});
        label->setToolTip(tr("Which side the picture shows"));
        buttons->addWidget(label);
        connect(b, &QPushButton::clicked, this, [this, index, id, label] {
          const QString file = QFileDialog::getOpenFileName(this, tr("Picture of the back or a side"), QString(),
                                                            tr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp)"));
          if (file.isEmpty()) return;
          QString error;
          if (!answer(index, id, file, label->currentText(), &error)) live_->setText(error);
        });
      } else {
        connect(b, &QPushButton::clicked, this, [this, index, id] {
          QString error;
          if (!answer(index, id, {}, {}, &error)) live_->setText(error);
        });
      }
      buttons->addWidget(b);
    }
    buttons->addStretch(1);
    lay->addLayout(buttons);
  } else if (it.state == "open") {
    lay->addWidget(wrapped(model_.finished() ? tr("Not answered: the run ended.") : tr("Waiting for an answer."), "secondary", card));
  }
  if (!it.answer.isEmpty()) lay->addWidget(wrapped(it.answer, nullptr, card));
  if (!it.outcome.isEmpty()) lay->addWidget(wrapped(it.outcome, "secondary", card));
  if (compared) {
    // One row: before (left) and after (right), the same three views.
    QVector<AgentThumb> both;
    for (auto t : it.before) {
      t.caption = tr("before");
      t.badge.clear();
      both.push_back(t);
    }
    for (auto t : it.after) {
      t.caption = tr("after");
      t.badge.clear();
      both.push_back(t);
    }
    lay->addWidget(make_label(tr("BEFORE / AFTER — BACK, BACK-LEFT, BACK-RIGHT"), "section", card));
    lay->addWidget(thumbRow(both, card, 196));
  }
  return card;
}

QWidget* AgentPage::buildFinal(QWidget* card) {
  auto* lay = static_cast<QVBoxLayout*>(card->layout());
  const auto& r = model_.report();
  for (const auto& d : r.value("deliverables", json::array()))
    lay->addWidget(wrapped(tr("Deliverable: %1 · %2 bytes · SHA-256 %3").arg(QDir::toNativeSeparators(qj(d, "path")), qj(d, "bytes"), qj(d, "sha256")),
                           nullptr, card));
  const auto m = r.value("measured", json::object());
  const auto scene = m.value("godot_scene", json::object());
  QString godot = tr("Godot: %1").arg(qj(m, "godot_status"));
  if (qj(m, "godot_status") == "PASS") {
    QStringList size;
    for (const auto& v : m.value("delivered_size_m", json::array()))
      if (v.is_number()) size << QString::number(v.get<double>(), 'f', 3);
    godot += tr(" · imported %1 triangles, %2 material(s), size %3 m · scale error %4 %")
                 .arg(qj(scene, "triangles"), qj(scene, "materials"), size.join(QStringLiteral(" × ")),
                      QString::number(m.value("scale_error_pct", -1.0), 'f', 3));
  }
  lay->addWidget(wrapped(godot, nullptr, card));
  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(theme::space::md);
  int row = 0;
  for (const auto& c : r.value("acceptance", json::array())) {
    grid->addWidget(make_label(qj(c, "criterion"), "small", card), row, 0);
    grid->addWidget(make_chip(qj(c, "status"), tone_of_status(qj(c, "status")), card), row, 1, Qt::AlignLeft);
    grid->addWidget(wrapped(qj(c, "detail"), "small", card), row, 2);
    ++row;
  }
  grid->setColumnStretch(2, 1);
  lay->addLayout(grid);
  for (const auto& d : r.value("human_decisions", json::array()))
    lay->addWidget(wrapped(tr("Human decision %1: \"%2\" by %3 at %4 — %5")
                               .arg(qj(d["question"], "id"), qj(d["answer"], "choice"), qj(d["answer"], "who"), qj(d["answer"], "when"), qj(d, "outcome")),
                           "secondary", card));
  for (const auto& p : r.value("pending_decisions", json::array()))
    lay->addWidget(wrapped(tr("PENDING_DECISION %1: %2").arg(qj(p["question"], "id"), qj(p["question"], "title")), "secondary", card));
  return card;
}

bool AgentPage::answer(int index, const QString& choice, const QString& picture, const QString& label, QString* error) {
  if (index < 0 || index >= model_.items().size() || model_.items()[index].kind != AgentItem::Kind::Question) return false;
  const auto& it = model_.items()[index];
  const QString dir = QDir(model_.dir()).filePath("decisions");
  const QString file = QDir(dir).filePath(it.key + ".answer.json");
  if (QFileInfo::exists(file)) {
    if (error) *error = tr("Question %1 is already answered.").arg(it.key);
    return false;
  }
  json a = {{"id", it.key.toStdString()}, {"choice", choice.toStdString()}, {"source", "app"},
            {"who", (qEnvironmentVariable("USERNAME").isEmpty() ? QStringLiteral("unknown user") : qEnvironmentVariable("USERNAME")).toStdString()},
            {"when", QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toStdString()}};
  if (!picture.isEmpty()) {
    a["picture"] = QDir::toNativeSeparators(QFileInfo(picture).absoluteFilePath()).toUtf8().toStdString();
    a["label"] = label.toStdString();
  }
  QSaveFile f(file);  // temp file + rename: the agent never reads half an answer
  if (!f.open(QIODevice::WriteOnly)) {
    if (error) *error = tr("Cannot write %1").arg(QDir::toNativeSeparators(file));
    return false;
  }
  const auto text = a.dump(1);
  f.write(text.data(), qint64(text.size()));
  if (!f.commit()) {
    if (error) *error = tr("Cannot write %1").arg(QDir::toNativeSeparators(file));
    return false;
  }
  live_->setText(tr("Answer sent: %1 — the agent continues").arg(choice));
  return true;
}

void AgentPage::scrollTo(const QString& where) {
  content_->adjustSize();
  QCoreApplication::processEvents();
  int target = -1;
  if (where == "final" || where == "question") {
    for (int i = 0; i < model_.items().size(); ++i)
      if ((where == "final" && model_.items()[i].kind == AgentItem::Kind::Final) ||
          (where == "question" && model_.items()[i].kind == AgentItem::Kind::Question)) {
        target = i;
        if (where == "question") break;
      }
  } else if (where != "top") {
    bool ok = false;
    target = where.toInt(&ok);
    if (!ok) target = -1;
  }
  if (target >= 0 && target < cards_.size() && cards_[target]) scroll_->verticalScrollBar()->setValue(cards_[target]->y() - 8);
  else scroll_->verticalScrollBar()->setValue(0);
}

}  // namespace p3dgui
