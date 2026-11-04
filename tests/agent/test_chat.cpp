// Tests of the milestone-4 chat agent (apps/agent/chat): words → spec fields (AR + EN), the rules brain's tool calls,
// the conversation log, answers to the agent's questions, picture labels and turnaround sheets.
// The agent process is replaced by a recorder (no run is simulated: run states come from fixture files).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <fstream>

#include "agent/agent_spec.hpp"
#include "chat.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

using namespace p3dchat;

namespace {

fs::path scratch(const std::string& name) {
  const auto dir = fs::temp_directory_path() / "3re-chat-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

// A figure-shaped opaque picture (taller than wide) on transparency; `figures` side by side for a sheet.
fs::path picture(const fs::path& file, int figures = 1, pixel3d::Rgba8 c = {200, 60, 40, 255}) {
  pixel3d::ImageRGBA img(60 * figures + 20 * (figures + 1), 160);
  for (int k = 0; k < figures; ++k)
    for (int y = 20; y < 140; ++y)
      for (int x = 20 + k * 80; x < 80 + k * 80; ++x) img.at(x, y) = c;
  fs::create_directories(file.parent_path());
  pixel3d::save_png(img, file);
  return file;
}

std::vector<std::string> calls_of(const std::vector<Message>& ms) {
  std::vector<std::string> out;
  for (const auto& m : ms)
    for (const auto& c : m.calls) out.push_back(c.name);
  return out;
}

std::string all_text(const std::vector<Message>& ms) {
  std::string t;
  for (const auto& m : ms)
    if (m.role == "assistant") t += m.text + "\n";
  return t;
}

struct Rig {
  fs::path dir;
  std::vector<std::pair<fs::path, fs::path>> started;  // (spec, out)
  uint64_t free_mib = 8000;
  int agents = 0;
  Tools tools;
  RulesProvider rules;
  Session session;
  explicit Rig(const std::string& name)
      : dir(scratch(name)),
        tools(ToolEnv{dir / "conv", dir / "runs", dir / "3re-agent.exe", "tester", dir / "projects",
                      [this](const fs::path& spec, const fs::path& out, std::string&) {
                        started.push_back({spec, out});
                        fs::create_directories(out);
                        return true;
                      },
                      [this] { return free_mib; }, [this] { return agents; }}),
        session(dir / "conv", rules, tools) {}
};

json read_log(const fs::path& f) {
  json lines = json::array();
  std::ifstream in(f, std::ios::binary);
  std::string l;
  while (std::getline(in, l))
    if (!l.empty()) lines.push_back(json::parse(l));
  return lines;
}

}  // namespace

TEST_CASE("words: English request → spec fields") {
  Draft d;
  read_words("Make this sword game-ready for PC, 8k triangles, 1.1 m long, low-poly cel-shaded style", d);
  CHECK(d.asset_type == std::optional<std::string>("prop"));
  CHECK(d.platform == std::optional<std::string>("pc"));
  CHECK(d.triangle_budget == std::optional<int>(8000));
  CHECK(d.metres == std::optional<double>(1.1));
  CHECK(d.axis == std::optional<std::string>("longest"));
  CHECK(d.style == "low-poly, cel-shaded");
}

TEST_CASE("words: Arabic request with Arabic-Indic digits") {
  Draft d;
  read_words("أريد هذه الشخصية جاهزة للجوال بطول ١٫٨ متر وأسلوب فانتازيا مظلمة", d);
  CHECK(d.asset_type == std::optional<std::string>("character"));
  CHECK(d.platform == std::optional<std::string>("mobile"));
  CHECK(d.metres == std::optional<double>(1.8));
  CHECK(d.style == "فانتازيا مظلمة");
  Draft t;
  read_words("سيف بميزانية ٥٠٠٠ مثلث", t);
  CHECK(t.triangle_budget == std::optional<int>(5000));
  CHECK_FALSE(t.metres.has_value());  // "مثلث" is not metres
  Draft r;
  read_words("هل يمكنك عمل ريغ للشخصية؟", r);
  CHECK(r.wants_rigging);
}

TEST_CASE("rules brain asks only for the missing required field") {
  Rig rig("ask-missing");
  auto out = rig.session.user("make it game ready");
  CHECK(calls_of(out).empty());
  CHECK(all_text(out).find("Attach") != std::string::npos);
  const auto pic = picture(rig.dir / "in" / "knight.png");
  out = rig.session.user("for mobile", {{pic, ""}});
  CHECK(calls_of(out).empty());
  CHECK(all_text(out).find("character or a prop") != std::string::npos);
  out = rig.session.user("it is a character");
  CHECK(all_text(out).find("How tall") != std::string::npos);
  CHECK(rig.started.empty());
}

TEST_CASE("rules brain: complete request → labels, spec, validation, run (real spec, logged calls)") {
  Rig rig("full-flow");
  const auto front = picture(rig.dir / "in" / "hero_front.png");
  const auto back = picture(rig.dir / "in" / "hero_back.webp.png", 1, {30, 30, 30, 255});
  const auto out = rig.session.user("Game-ready character for mobile, 1.8 m tall, dark fantasy style", {{back, ""}, {front, ""}});
  const auto calls = calls_of(out);
  REQUIRE(calls == std::vector<std::string>{"propose_labels", "make_spec", "validate_spec", "run_agent"});
  REQUIRE(rig.started.size() == 1);
  const auto spec = p3dagent::load_spec(rig.started[0].first);
  REQUIRE(spec.spec.has_value());
  CHECK(spec.spec->type == p3dagent::AssetType::Character);
  CHECK(spec.spec->preset == "mobile_character");
  CHECK(spec.spec->triangle_budget == 5000);
  CHECK(spec.spec->scale_metres == doctest::Approx(1.8));
  CHECK(spec.spec->requested_style == "dark fantasy");
  REQUIRE(spec.spec->views.size() == 1);
  CHECK(spec.spec->views[0].label == "back");
  CHECK(pixel3d::to_utf8(spec.spec->picture.filename()).find("hero_front") != std::string::npos);
  // The person's files are copied, never used in place.
  CHECK(spec.spec->picture.parent_path() == rig.dir / "conv" / "inputs");
  const std::string text = all_text(out);
  CHECK(text.find("NOT applied") != std::string::npos);
  CHECK(text.find("proposals") != std::string::npos);
  // Every call and its result is in the conversation's run.jsonl.
  const auto log = read_log(rig.session.log_file());
  int tool_calls = 0, tool_results = 0;
  for (const auto& l : log) {
    tool_calls += l.value("event", "") == "tool_call";
    tool_results += l.value("event", "") == "tool_result";
    CHECK(l.contains("timestamp"));
  }
  CHECK(tool_calls == 4);
  CHECK(tool_results == 4);
}

TEST_CASE("unordered pictures without names: the brain asks for the front, then labels are left to the registration") {
  Rig rig("unordered");
  const auto a = picture(rig.dir / "in" / "IMG_0412.png"), b = picture(rig.dir / "in" / "IMG_0413.png");
  auto out = rig.session.user("pc character 1.7 m", {{a, ""}, {b, ""}});
  CHECK(calls_of(out) == std::vector<std::string>{"propose_labels"});
  CHECK(all_text(out).find("Which picture is the front") != std::string::npos);
  out = rig.session.user("the front is IMG_0413.png");
  REQUIRE(calls_of(out) == std::vector<std::string>{"propose_labels", "make_spec", "validate_spec", "run_agent"});
  const auto spec = p3dagent::load_spec(rig.started.at(0).first);
  REQUIRE(spec.spec.has_value());
  CHECK(pixel3d::to_utf8(spec.spec->picture.filename()).find("IMG_0413") != std::string::npos);
  REQUIRE(spec.spec->views.size() == 1);
  CHECK(spec.spec->views[0].label == "auto");
}

TEST_CASE("turnaround sheet is split; its first figure is the front") {
  Rig rig("sheet");
  const auto sheet = picture(rig.dir / "in" / "turnaround.png", 4);
  const auto r = rig.tools.propose_labels({{"pictures", {pixel3d::to_utf8(sheet)}}});
  REQUIRE(r.value("ok", false));
  CHECK(r["sheets"].size() == 1);
  CHECK(r["sheets"][0]["parts"].size() == 4);
  CHECK_FALSE(r.value("needs_front", true));
  CHECK(r["views"].size() == 3);
  CHECK(r.value("front", std::string()).find("turnaround-1") != std::string::npos);
}

TEST_CASE("labels from file names, English and Arabic") {
  Rig rig("labels");
  const auto r = rig.tools.propose_labels({{"pictures",
                                            {pixel3d::to_utf8(picture(rig.dir / "in" / "أمام.png")), pixel3d::to_utf8(picture(rig.dir / "in" / "خلف.png")),
                                             pixel3d::to_utf8(picture(rig.dir / "in" / "char_left.png")), pixel3d::to_utf8(picture(rig.dir / "in" / "side.png"))}}});
  REQUIRE(r.value("ok", false));
  CHECK(r.value("front", std::string()).find("أمام") != std::string::npos);
  std::map<std::string, std::string> got;
  for (const auto& v : r["views"]) got[pixel3d::to_utf8(pixel3d::path_from_utf8(v["original"].get<std::string>()).stem())] = v["label"];
  CHECK(got["خلف"] == "back");
  CHECK(got["char_left"] == "left");
  CHECK(got["side"] == "auto");
}

TEST_CASE("the agent's question reaches the chat; the person's answer is written with who and source") {
  Rig rig("answer");
  const auto front = picture(rig.dir / "in" / "vampire_front.png");
  rig.session.user("pc character 1.9 m", {{front, ""}});
  REQUIRE(rig.started.size() == 1);
  const fs::path run = rig.started[0].second;
  // Fixture: what the agent writes when it asks (format of agent_decide.cpp).
  fs::create_directories(run / "decisions");
  {
    std::ofstream q(run / "decisions" / "q1.question.json", std::ios::binary);
    q << json{{"id", "q1"}, {"kind", "budget_or_fidelity"}, {"title", "The budget cannot be met within fidelity"},
              {"why", "fidelity FAIL at 30,000 triangles"}, {"asked_at", "2026-10-31T10:00:00"},
              {"options", {{{"id", "budget"}, {"label", "Deliver at the budget"}}, {{"id", "needed"}, {"label", "Deliver at 48,344 triangles"}}}}}
             .dump();
  }
  const auto state = read_run_state(run);
  REQUIRE(state.value("state", "") == "waiting");
  auto out = rig.session.event(state);
  CHECK(all_text(out).find("needs your decision") != std::string::npos);
  CHECK(all_text(out).find("needed") != std::string::npos);
  out = rig.session.user("use more triangles, the needed count");
  REQUIRE(calls_of(out) == std::vector<std::string>{"answer_decision"});
  const fs::path ans = run / "decisions" / "q1.answer.json";
  REQUIRE(fs::exists(ans));
  std::ifstream in(ans, std::ios::binary);
  const auto a = json::parse(in);
  CHECK(a.value("choice", "") == "needed");
  CHECK(a.value("who", "") == "tester");
  CHECK(a.value("source", "") == "chat");
}

TEST_CASE("finished run: the brain quotes the measured result and never calls it polished without real pictures") {
  Rig rig("finished");
  const fs::path run = rig.dir / "runs" / "sword-1";
  fs::create_directories(run);
  {
    std::ofstream r(run / "report.json", std::ios::binary);
    r << json{{"status", "PASS"}, {"summary", "7678 triangles"},
              {"acceptance", {{{"criterion", "triangle_budget"}, {"status", "PASS"}, {"detail", "7678 / 8000"}},
                              {{"criterion", "fidelity"}, {"status", "PASS"}, {"detail", "ok"}},
                              {{"criterion", "texture"}, {"status", "PASS"}, {"detail", "ok"}},
                              {{"criterion", "godot_import"}, {"status", "PASS"}, {"detail", "ok"}}}},
              {"deliverables", {{{"path", "sword.glb"}, {"sha256", "70c4d36a00000000"}}}}}
             .dump();
  }
  const auto out = rig.session.event(read_run_state(run));
  const auto text = all_text(out);
  CHECK(text.find("measured by the agent") != std::string::npos);
  CHECK(text.find("PASS") != std::string::npos);
  CHECK(text.find("Polished by the measured definition") != std::string::npos);
  CHECK(text.find(": no") != std::string::npos);  // no side painted from a real picture
}

TEST_CASE("rigging request: the brain says it does not exist, and starts nothing") {
  Rig rig("rigging");
  const auto out = rig.session.user("can you rig my character with a skeleton?");
  CHECK(calls_of(out).empty());
  CHECK(all_text(out).find("does not rig") != std::string::npos);
}

TEST_CASE("fake provider: scripted tool calls go through the real tools and are logged") {
  const auto dir = scratch("fake");
  Tools tools(ToolEnv{dir / "conv", dir / "runs", {}, "tester", {}, [](const fs::path&, const fs::path&, std::string&) { return true; },
                      [] { return uint64_t(8000); }, [] { return 0; }});
  FakeProvider fake({{"", {{"", "make_spec", {{"asset_type", "prop"}, {"metres", 0.8}, {"triangle_budget", 1500}, {"glb", "C:/x/crate.glb"}}}}},
                     {"spec written", {}}});
  Session s(dir / "conv", fake, tools);
  const auto out = s.user("crate");
  CHECK(calls_of(out) == std::vector<std::string>{"make_spec"});
  bool ok = false;
  for (const auto& m : out)
    if (m.role == "tool") ok = m.result.value("ok", false);
  CHECK(ok);
  CHECK(fake.remaining() == 0);
}

TEST_CASE("spec: views and requested_style are validated") {
  json j = {{"schema", "3re.asset-spec/1"}, {"name", "c"}, {"input", {{"picture", "f.png"}, {"views", {{{"picture", "b.png"}, {"label", "upside"}}}}}},
            {"asset_type", "character"}, {"preset", "pc_character"}, {"scale", {{"metres", 1.8}}}, {"output_dir", "out"}};
  auto r = p3dagent::parse_spec(j, "C:/s");
  CHECK_FALSE(r.spec.has_value());
  j["input"]["views"][0]["label"] = "back";
  j["requested_style"] = "toon";
  r = p3dagent::parse_spec(j, "C:/s");
  REQUIRE(r.spec.has_value());
  CHECK(r.spec->views.size() == 1);
  CHECK(r.spec->requested_style == "toon");
  CHECK(p3dagent::to_json(*r.spec)["requested_style"] == "toon");
}

TEST_CASE("W5 admission: one agent run at a time, free RAM measured before the start") {
  CHECK(admit_run(true, 8000, 0).start);
  const auto busy = admit_run(false, 8000, 1);
  CHECK_FALSE(busy.start);
  CHECK(busy.reason.find("another agent run") != std::string::npos);
  const auto low = admit_run(true, 1400, 0);
  CHECK_FALSE(low.start);
  CHECK(low.needed_mib == 3328);
  CHECK(low.reason.find("1400 MiB are free") != std::string::npos);
  CHECK(admit_run(false, 2100, 0).start);  // reusing a generation needs less than generating
}

TEST_CASE("W5 in the chat: the run waits with the measured reason, and starts when the person says so") {
  Rig rig("wait");
  rig.free_mib = 1400;
  const auto pic = picture(rig.dir / "in" / "robot_front.png");
  auto out = rig.session.user("pc character 1.6 m", {{pic, ""}});
  CHECK(rig.started.empty());
  CHECK(all_text(out).find("not ready (measured)") != std::string::npos);
  CHECK(all_text(out).find("1400 MiB are free") != std::string::npos);
  rig.free_mib = 6000;
  out = rig.session.user("start");
  CHECK(calls_of(out) == std::vector<std::string>{"run_agent"});
  CHECK(rig.started.size() == 1);
}

TEST_CASE("W4 words: a typed mark is read; a problem without a mark asks for the views") {
  Draft d;
  read_words("the eye is smeared, mark yaw=0 x=235 y=117 r=7", d);
  REQUIRE(d.mark.has_value());
  CHECK((*d.mark)["x"].get<double>() == doctest::Approx(235));
  CHECK((*d.mark)["r"].get<double>() == doctest::Approx(7));
  Rig rig("problem");
  const fs::path run = rig.dir / "runs" / "anime-1";
  fs::create_directories(run);
  { std::ofstream r(run / "report.json", std::ios::binary); r << json{{"status", "PASS"}, {"summary", "ok"}}.dump(); }
  rig.session.event(read_run_state(run));
  // The run is known to the conversation through the event: a mark goes to regional_fix with that run.
  json state = read_run_state(run);
  state["run"] = pixel3d::to_utf8(run);
  rig.session.event(state);
  const auto out = rig.session.user("there is a problem here: the eye is smeared, mark yaw=0 x=235 y=117 r=7");
  REQUIRE(calls_of(out) == std::vector<std::string>{"regional_fix"});
  // The fake run has no textured model: the tool says so (no fix is invented).
  bool refused = false;
  for (const auto& m : out)
    if (m.role == "tool") refused = !m.result.value("ok", true) && m.result.value("error", std::string()).find("textured model") != std::string::npos;
  CHECK(refused);
}

TEST_CASE("review fixes: 'hero asset' is a platform, not a character; 'high detail' is not a height") {
  Draft d;
  read_words("a sword", d);
  read_words("hero asset, high detail", d);
  CHECK(d.asset_type == std::optional<std::string>("prop"));
  CHECK(d.platform == std::optional<std::string>("hero"));
  CHECK_FALSE(d.axis.has_value());
}

TEST_CASE("review fixes: a picture option is not chosen without a picture, and the tool refuses it") {
  Rig rig("picture-answer");
  const auto front = picture(rig.dir / "in" / "hood_front.png");
  rig.session.user("pc character 1.8 m", {{front, ""}});
  const fs::path run = rig.started.at(0).second;
  fs::create_directories(run / "decisions");
  {
    std::ofstream q(run / "decisions" / "q1.question.json", std::ios::binary);
    q << json{{"id", "q1"}, {"kind", "hidden_side_picture"}, {"title", "The back is a guess"}, {"why", "flat back"}, {"asked_at", "2026-10-31T10:00:00"},
              {"options", {{{"id", "picture"}, {"label", "Use my picture"}, {"needs_picture", true}}, {{"id", "keep"}, {"label", "Keep"}}}}}
             .dump();
  }
  rig.session.event(read_run_state(run));
  auto out = rig.session.user("I will send the back picture later");
  for (const auto& c : calls_of(out)) CHECK(c != "answer_decision");
  CHECK_FALSE(fs::exists(run / "decisions" / "q1.answer.json"));
  const auto r = rig.tools.answer_decision({{"run", pixel3d::to_utf8(run)}, {"question", "q1"}, {"choice", "picture"}});
  CHECK_FALSE(r.value("ok", true));
  CHECK(r.value("error", std::string()).find("needs a picture") != std::string::npos);
}

TEST_CASE("review fixes: parts of a turnaround sheet never take a label from the sheet's name") {
  Rig rig("sheet-name");
  const auto sheet = picture(rig.dir / "in" / "knight_front_side_back.png", 3);
  const auto r = rig.tools.propose_labels({{"pictures", {pixel3d::to_utf8(sheet)}}});
  REQUIRE(r.value("ok", false));
  for (const auto& v : r["views"]) CHECK(v.value("label", "") == "auto");
}

TEST_CASE("W5 reattach: a conversation folder reopens with its messages and its run") {
  const auto dir = scratch("reopen");
  std::vector<fs::path> started;
  auto env = ToolEnv{dir / "conv", dir / "runs", {}, "tester", {},
                     [&](const fs::path&, const fs::path& out, std::string&) { started.push_back(out); fs::create_directories(out); return true; },
                     [] { return uint64_t(8000); }, [] { return 0; }};
  size_t before = 0;
  fs::path run;
  {
    Tools tools(env);
    RulesProvider rules;
    Session s(dir / "conv", rules, tools);
    s.user("pc prop 0.9 m", {{picture(dir / "in" / "axe_front.png"), ""}});
    before = s.messages().size();
    run = s.current_run();
    REQUIRE_FALSE(run.empty());
  }
  Tools tools(env);
  RulesProvider rules;
  Session again(dir / "conv", rules, tools);  // the app was closed and opened again
  CHECK(again.messages().size() == before);
  CHECK(again.current_run() == run);
  const auto out = again.user("status");
  CHECK(calls_of(out) == std::vector<std::string>{"run_status"});
}

TEST_CASE("guard: in a turn started by the agent, no brain can answer the agent's question for the person") {
  const auto dir = scratch("guard");
  std::vector<fs::path> started;
  Tools tools(ToolEnv{dir / "conv", dir / "runs", {}, "tester", {},
                      [&](const fs::path&, const fs::path& out, std::string&) { started.push_back(out); fs::create_directories(out); return true; },
                      [] { return uint64_t(8000); }, [] { return 0; }});
  const fs::path run = dir / "runs" / "r1";
  fs::create_directories(run / "decisions");
  {
    std::ofstream q(run / "decisions" / "q1.question.json", std::ios::binary);
    q << json{{"id", "q1"}, {"kind", "budget_or_fidelity"}, {"title", "t"}, {"why", "w"}, {"asked_at", "2026-11-01T10:00:00"},
              {"options", {{{"id", "budget"}, {"label", "b"}}, {{"id", "needed"}, {"label", "n"}}}}}
             .dump();
  }
  // A brain that answers on its own as soon as the question arrives (what Qwen3-VL-4B did in the evaluation).
  FakeProvider eager({{"", {{"", "answer_decision", {{"run", pixel3d::to_utf8(run)}, {"question", "q1"}, {"choice", "needed"}}}}}, {"done", {}}});
  Session s(dir / "conv", eager, tools);
  const auto out = s.event(read_run_state(run));
  bool refused = false;
  for (const auto& m : out)
    if (m.role == "tool") refused = !m.result.value("ok", true) && m.result.value("error", std::string()).find("only the person") != std::string::npos;
  CHECK(refused);
  CHECK_FALSE(fs::exists(run / "decisions" / "q1.answer.json"));
}

namespace {
// A finished run as the agent leaves it: report with the generation it used and the extra pictures' verdicts.
fs::path finished_run(Rig& rig, const json& pictures) {
  const fs::path gen = rig.dir / "gen";
  fs::create_directories(gen);
  { std::ofstream g(gen / "geometry.glb", std::ios::binary); g << "glTF"; }
  { std::ofstream n(gen / "native.post", std::ios::binary); n << "post"; }
  std::vector<Attachment> att{{picture(rig.dir / "in" / "hero_front.png"), ""}};
  if (!pictures.empty()) att.push_back({picture(rig.dir / "in" / "hero_left.png", 1, {20, 20, 20, 255}), ""});  // the extra picture of the spec
  rig.session.user("pc character 1.8 m", att);
  const fs::path run = rig.started.back().second;
  fs::create_directories(run / "work");
  picture(run / "work" / "view-1-auto.png", 1, {20, 20, 20, 255});
  std::ofstream r(run / "report.json", std::ios::binary);
  r << json{{"status", "PASS"}, {"summary", "ok"}, {"source", {{"run", pixel3d::to_utf8(gen)}}}, {"pictures", pictures},
            {"acceptance", {{{"criterion", "texture"}, {"status", "PASS"}, {"detail", "ok"}}}}, {"deliverables", {{{"path", "m.glb"}, {"sha256", "00"}}}}}
           .dump();
  return run;
}
json spec_of_last_start(Rig& rig) {
  std::ifstream in(rig.started.back().first, std::ios::binary);
  return json::parse(in);
}
}  // namespace

TEST_CASE("rejected picture: the brain asks which side, and paints again with the side the person names") {
  Rig rig("rejected-side");
  const fs::path run = finished_run(rig, {{{"name", "view-1-left.png"}, {"label", "left"}, {"status", "rejected"}, {"reason", "fits at 288 degrees"}}});
  auto out = rig.session.event(read_run_state(run));
  CHECK(all_text(out).find("Which side does it show") != std::string::npos);
  out = rig.session.user("it is the back");
  REQUIRE(calls_of(out) == std::vector<std::string>{"retexture_with_pictures"});
  REQUIRE(rig.started.size() == 2);
  const json spec = spec_of_last_start(rig);
  CHECK(spec["input"].value("reuse_run", std::string()) == pixel3d::to_utf8(rig.dir / "gen"));  // same generation
  REQUIRE(spec["input"]["views"].size() == 1);  // corrected, not added next to the wrong one
  CHECK(spec["input"]["views"][0]["label"] == "back");
  CHECK(spec["input"]["views"][0]["picture"].get<std::string>().find("hero_left") != std::string::npos);
  CHECK(p3dagent::load_spec(rig.started.back().first).spec.has_value());
  CHECK(all_text(out).find("Painting the same model again") != std::string::npos);
}

TEST_CASE("more pictures for a finished model: re-texture with them (the side from the words, else auto)") {
  Rig rig("more-pictures");
  const fs::path run = finished_run(rig, json::array());
  rig.session.event(read_run_state(run));
  const auto out = rig.session.user("here is the back of the character", {{picture(rig.dir / "in" / "IMG_9.png", 1, {30, 30, 30, 255}), ""}});
  REQUIRE(calls_of(out) == std::vector<std::string>{"retexture_with_pictures"});
  const json spec = spec_of_last_start(rig);
  REQUIRE(spec["input"]["views"].size() == 1);
  CHECK(spec["input"]["views"][0]["label"] == "back");
  CHECK(spec.value("triangle_budget", 0) == 0);  // the budget still comes from the same preset
  CHECK(spec.value("preset", std::string()) == "pc_character");
}

TEST_CASE("2.10.0: without Godot the chat does not start a run, says why and how to fix it, and starts on \"start\" once found") {
  const auto dir = scratch("no-godot");
  std::vector<fs::path> started;
  bool godot_installed = false;
  auto env = ToolEnv{dir / "conv", dir / "runs", {}, "tester", {},
                     [&](const fs::path&, const fs::path& out, std::string&) { started.push_back(out); fs::create_directories(out); return true; },
                     [] { return uint64_t(8000); }, [] { return 0; }};
  env.find_godot = [&] {
    p3dagent::GodotLocation g;
    if (godot_installed) {
      g.exe = "C:/Godot/Godot_v4.7-stable_win64_console.exe";
      g.version = "4.7.stable.official";
      g.source = "found automatically";
    }
    return g;
  };
  Tools tools(env);
  RulesProvider rules;
  Session s(dir / "conv", rules, tools);
  auto out = s.user("pc prop 0.9 m", {{picture(dir / "in" / "axe_front.png"), ""}});
  CHECK(started.empty());
  CHECK(all_text(out).find("Godot 4 was not found") != std::string::npos);
  CHECK(all_text(out).find("Settings > Delivery check") != std::string::npos);
  auto ar = s.user("ابدأ");  // still missing: the same message, in Arabic
  CHECK(started.empty());
  CHECK(all_text(ar).find("لم أجد Godot 4") != std::string::npos);
  godot_installed = true;
  out = s.user("start");
  REQUIRE(started.size() == 1);
  CHECK(all_text(out).find("Delivery check: Godot 4.7.stable.official") != std::string::npos);
}
