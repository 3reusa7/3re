// Agent run viewer (docs/agent/PLAN-M3.md, part 2): timeline from run.jsonl, live appends, answer buttons, final card.
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <nlohmann/json.hpp>

#include "registry.hpp"
#include "studio/agent_chat_panel.hpp"
#include "studio/agent_page.hpp"
#include "studio/agent_run_model.hpp"

using nlohmann::json;

namespace {

void append(const QString& dir, const json& line) {
  QFile f(QDir(dir).filePath("run.jsonl"));
  QVERIFY(f.open(QIODevice::Append));
  const auto s = line.dump() + "\n";
  f.write(s.data(), qint64(s.size()));
}

QString png(const QString& dir, const QString& name) {
  QImage img(32, 32, QImage::Format_RGB32);
  img.fill(Qt::gray);
  const QString f = QDir(dir).filePath(name);
  img.save(f);
  return f;
}

json question(const QString& dir) {
  return {{"id", "q1"},
          {"kind", "budget_or_fidelity"},
          {"title", "The triangle budget cannot be met within fidelity"},
          {"why", "At the budget the model no longer looks like the original."},
          {"numbers", {{"budget", 30000}, {"original_triangles", 150000}, {"at_budget", {{"triangles", 29900}}}, {"needed", 47300}}},
          {"renders", {{{"path", png(dir, "o.png").toStdString()}, {"group", "original"}, {"caption", "original"}},
                       {{"path", png(dir, "b.png").toStdString()}, {"group", "budget"}, {"caption", "at the budget"}}}},
          {"options", {{{"id", "budget"}, {"label", "Deliver at the budget"}, {"detail", ""}, {"needs_picture", false}},
                       {{"id", "needed"}, {"label", "Deliver at 47300 triangles"}, {"detail", ""}, {"needs_picture", false}}}}};
}

}  // namespace

class AgentPageTests : public QObject {
  Q_OBJECT
 private slots:
  void model_reads_complete_lines_and_appends_live() {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    append(dir, {{"timestamp", "2026-11-01T10:00:00.000"}, {"event", "start"}, {"spec", {{"name", "vampire"}, {"triangle_budget", 30000}}}});
    append(dir, {{"timestamp", "2026-11-01T10:00:01.000"}, {"event", "step_started"}, {"step", "inspect"}, {"tool", "pixel3d-asset-inspect"}});
    p3dgui::AgentRunModel m;
    QVERIFY(m.open(dir));
    QCOMPARE(m.poll().size(), 1);
    QCOMPARE(m.name(), QString("vampire"));
    QCOMPARE(m.runningTool(), QString("pixel3d-asset-inspect"));
    // A half-written line is not read until it is complete.
    QFile f(QDir(dir).filePath("run.jsonl"));
    QVERIFY(f.open(QIODevice::Append));
    f.write(R"({"timestamp":"2026-11-01T10:00:05.000","step":"inspect","tool":"pixel3d-asset-inspect","exit_code":0,"seconds":4.0)");
    f.flush();
    QCOMPARE(m.poll().size(), 0);
    f.write(",\"peak_memory_mib\":120}\n");
    f.close();
    const auto changed = m.poll();
    QCOMPARE(changed.size(), 1);
    QCOMPARE(m.items()[changed[0]].kind, p3dgui::AgentItem::Kind::Step);
    QVERIFY(m.runningTool().isEmpty());
    QCOMPARE(m.items()[changed[0]].at, 5.0);
  }

  void candidates_are_marked_chosen_or_rejected() {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    const QString t = png(dir, "g.png");
    append(dir, {{"timestamp", "2026-11-01T10:00:00.000"}, {"event", "decimation_candidate"}, {"name", "guarded QEM"}, {"ran", true},
                 {"triangles", 47696}, {"budget_met", false}, {"fidelity", {{"status", "FAIL"}}}, {"thumbnail", t.toStdString()}});
    append(dir, {{"timestamp", "2026-11-01T10:00:01.000"}, {"event", "decimation_candidate"}, {"name", "QEM without the crossing guard"}, {"ran", true},
                 {"triangles", 29830}, {"budget_met", true}, {"fidelity", {{"status", "PASS"}}}, {"thumbnail", t.toStdString()}});
    append(dir, {{"timestamp", "2026-11-01T10:00:02.000"}, {"event", "decimation_candidates"},
                 {"candidates", {{{"name", "guarded QEM"}, {"chosen", false}}, {{"name", "QEM without the crossing guard"}, {"chosen", true}}}}});
    p3dgui::AgentRunModel m;
    m.open(dir);
    m.poll();
    QCOMPARE(m.items().size(), 1);  // one card, candidates side by side
    QCOMPARE(m.items()[0].thumbs.size(), 2);
    QCOMPARE(m.items()[0].thumbs[0].badge, QString("rejected"));
    QCOMPARE(m.items()[0].thumbs[1].badge, QString("chosen"));
    QCOMPARE(m.items()[0].lines.size(), 2);
    QCOMPARE(m.items()[0].tone, QString("pass"));
    QVERIFY(m.items()[0].detail.contains("QEM without the crossing guard"));
  }

  void answer_button_writes_the_answer_file() {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    QDir().mkpath(QDir(dir).filePath("decisions"));
    append(dir, {{"timestamp", "2026-11-01T10:00:00.000"}, {"event", "start"}, {"spec", {{"name", "vampire"}}}});
    append(dir, {{"timestamp", "2026-11-01T10:01:00.000"}, {"event", "question"}, {"mode", "ask"}, {"question", question(dir)}});
    p3dgui::AgentPage page;
    page.openRun(dir);
    QCOMPARE(page.model().openQuestion(), 1);
    auto* b = page.findChild<QPushButton*>("agentAnswer_q1_needed");
    QVERIFY(b);
    QTest::mouseClick(b, Qt::LeftButton);
    QFile f(QDir(dir).filePath("decisions/q1.answer.json"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const auto a = json::parse(f.readAll().toStdString());
    QCOMPARE(QString::fromStdString(a["choice"]), QString("needed"));
    QCOMPARE(QString::fromStdString(a["source"]), QString("app"));
    QVERIFY(!a["who"].get<std::string>().empty());
    QVERIFY(!a["when"].get<std::string>().empty());
    // A second answer is refused (the first one stands).
    QString error;
    QVERIFY(!page.answer(1, "budget", {}, {}, &error));
    // The agent's answer line closes the question: no buttons any more.
    append(dir, {{"timestamp", "2026-11-01T10:01:30.000"}, {"event", "answer"}, {"question", "q1"},
                 {"answer", {{"choice", "needed"}, {"who", "sam"}, {"when", "2026-11-01T10:01:29"}, {"source", "app"}}}});
    page.refresh();
    QCoreApplication::processEvents();
    QCOMPARE(page.model().items()[1].state, QString("answered"));
    QCOMPARE(page.model().openQuestion(), -1);
  }

  void headless_questions_have_no_buttons_and_finish_with_the_report() {
    QTemporaryDir tmp;
    const QString dir = tmp.path();
    append(dir, {{"timestamp", "2026-11-01T10:00:00.000"}, {"event", "question"}, {"mode", "headless"}, {"question", question(dir)}});
    append(dir, {{"timestamp", "2026-11-01T10:00:00.100"}, {"event", "pending_decision"}, {"question", "q1"}, {"status", "PENDING_DECISION"},
                 {"why", "headless run"}});
    append(dir, {{"timestamp", "2026-11-01T10:05:00.000"}, {"event", "finished"}, {"status", "FAIL"}});
    QFile r(QDir(dir).filePath("report.json"));
    QVERIFY(r.open(QIODevice::WriteOnly));
    r.write(json{{"status", "FAIL"},
                 {"summary", "29918 triangles (budget 30000), fidelity FAIL; 1 decision(s) PENDING_DECISION"},
                 {"acceptance", {{{"criterion", "fidelity"}, {"status", "FAIL"}, {"detail", "x"}}}},
                 {"measured", {{"godot_status", "PASS"}, {"godot_scene", {{"triangles", 29918}, {"materials", 1}}}}},
                 {"pending_decisions", {{{"question", {{"id", "q1"}, {"title", "t"}}}}}}}
                .dump()
                .c_str());
    r.close();
    p3dgui::AgentPage page;
    page.openRun(dir);
    QVERIFY(!page.findChild<QPushButton*>("agentAnswer_q1_needed"));
    QCOMPARE(page.model().items()[0].state, QString("pending"));
    QVERIFY(page.model().finished());
    QVERIFY(page.model().reportLoaded());
    QCOMPARE(page.model().items().back().kind, p3dgui::AgentItem::Kind::Final);
    QCOMPARE(page.model().status(), QString("FAIL"));
  }
  // Milestone 4 (W3): the chat is on the Agent page; a complete request with pictures starts a run through the real
  // tools (the process start is replaced), the page shows it, and the conversation is logged.
  void chat_panel_starts_a_run_and_logs_the_conversation() {
    p3dgui::AgentPage page;
    QVERIFY(page.chat());
    QVERIFY(page.findChild<QWidget*>("agentChatInput"));
    QVERIFY(page.findChild<QWidget*>("agentChatAttach"));
    QTemporaryDir tmp;
    QStringList started;
    p3dgui::AgentChatPanel chat(QDir(tmp.path()).filePath("conv"), QDir(tmp.path()).filePath("runs"), QDir(tmp.path()).filePath("3re-agent.exe"),
                                nullptr, [&](const std::filesystem::path&, const std::filesystem::path& out, std::string&) {
                                  started << QString::fromStdWString(out.wstring());
                                  std::filesystem::create_directories(out);
                                  return true;
                                });
    QVERIFY(!QDir(QDir(tmp.path()).filePath("conv")).exists());  // nothing written before the first message
    QString shown;
    connect(&chat, &p3dgui::AgentChatPanel::runStarted, this, [&](const QString& d) { shown = d; });
    chat.attach({png(tmp.path(), "knight_front.png"), png(tmp.path(), "knight_back.png")});
    QCOMPARE(chat.pending().size(), 2);
    chat.send(QStringLiteral("شخصية للكمبيوتر بطول 1.8 متر"));
    QVERIFY(chat.pending().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!chat.busy(), 30000);  // the turn runs off the window's thread
    // The machine check uses the real free RAM: the run either started or waits with its measured reason.
    const auto* s = chat.session();
    QVERIFY(s);
    bool waited = false;
    for (const auto& m : s->messages())
      if (m.role == "tool" && m.tool == "run_agent") waited = m.result.value("waiting", false);
    if (!waited) {
      if (started.isEmpty())
        for (const auto& m : s->messages()) qWarning("%s", m.to_json().dump().substr(0, 400).c_str());
      QCOMPARE(started.size(), 1);
      QCOMPARE(shown, started.front());
    }
    QVERIFY(QFile::exists(QDir(chat.conversationDir()).filePath("run.jsonl")));
    QVERIFY(chat.messageCount() >= 6);  // you, labels, spec, validate, run (+ tool results)
  }
};
THREERE_TEST(AgentPageTests)
#include "test_agent_page.moc"
