// Brain evaluation (docs/agent/chat-eval/README.md, pre-registered): sets 1–3 through the real conversation engine
// and tools; the agent process start is recorded, run states come from fixture files. Prints one line per item and
// writes every conversation and a results JSON.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#include "chat.hpp"
#include "chat_eval.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dchat {

namespace {

std::string norm(std::string s) {
  std::string o;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 128 && !std::isalnum(u)) { if (!o.empty() && o.back() != ' ') o += ' '; continue; }
    o += u < 128 ? char(std::tolower(u)) : c;
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  return o;
}

struct Rig {
  fs::path dir;
  std::vector<fs::path> started;
  std::unique_ptr<Tools> tools;
  std::unique_ptr<Session> session;
  Rig(const fs::path& d, Provider& brain) : dir(d) {
    fs::remove_all(dir);
    ToolEnv env{dir / "conv", dir / "runs", dir / "3re-agent.exe", "evaluator", {},
                [this](const fs::path&, const fs::path& out, std::string&) { started.push_back(out); fs::create_directories(out); return true; },
                [] { return uint64_t(8000); }, [] { return 0; }};
    tools = std::make_unique<Tools>(env);
    session = std::make_unique<Session>(dir / "conv", brain, *tools);
  }
};

std::vector<std::string> calls_in(const std::vector<Message>& ms) {
  std::vector<std::string> out;
  for (const auto& m : ms)
    for (const auto& c : m.calls) out.push_back(c.name);
  return out;
}
const ToolCall* first_call(const std::vector<Message>& ms, const std::string& name) {
  for (const auto& m : ms)
    for (const auto& c : m.calls)
      if (c.name == name) return &c;
  return nullptr;
}
std::string reply_text(const std::vector<Message>& ms) {
  std::string t;
  for (const auto& m : ms)
    if (m.role == "assistant") t += m.text + "\n";
  return t;
}

void write_fixture(const fs::path& run, const std::string& kind) {
  if (kind == "report_pass") {
    std::ofstream r(run / "report.json", std::ios::binary);
    r << json{{"status", "PASS"}, {"summary", "29800 triangles (budget 30000), fidelity PASS, texture PASS, Godot PASS"},
              {"acceptance", {{{"criterion", "triangle_budget"}, {"status", "PASS"}, {"detail", "29800 / 30000"}},
                              {{"criterion", "godot_import"}, {"status", "PASS"}, {"detail", "imported"}}}},
              {"deliverables", {{{"path", "model.glb"}, {"sha256", "0000"}}}}}
             .dump();
    return;
  }
  fs::create_directories(run / "decisions");
  std::ofstream q(run / "decisions" / "q1.question.json", std::ios::binary);
  if (kind == "question_budget")
    q << json{{"id", "q1"}, {"kind", "budget_or_fidelity"}, {"title", "The triangle budget cannot be met within fidelity"},
              {"why", "At 30,000 triangles the model no longer looks like the original; it needs 48,344."},
              {"options", {{{"id", "budget"}, {"label", "Deliver at the budget with the measured loss"}},
                           {{"id", "needed"}, {"label", "Deliver at 48,344 triangles (fidelity PASS)"}}}}}
             .dump();
  else
    q << json{{"id", "q1"}, {"kind", "hidden_side_picture"}, {"title", "The back is TRELLIS's guess and looks weak"},
              {"why", "hidden sides are large flat colour patches"},
              {"options", {{{"id", "picture"}, {"label", "Use my picture of the back or a side"}, {"needs_picture", true}},
                           {{"id", "keep"}, {"label", "Keep the inferred back"}}}}}
             .dump();
}

}  // namespace

int run_brain_eval(Provider& brain, const fs::path& eval_dir, const fs::path& out, const fs::path& set2_dir) {
  fs::create_directories(out);
  // A neutral figure as the attached picture of sets 1 and 3.
  const fs::path pic = out / "fixtures" / "front.png";
  {
    pixel3d::ImageRGBA img(120, 240);
    for (int y = 20; y < 220; ++y)
      for (int x = 30; x < 90; ++x) img.at(x, y) = {150, 120, 100, 255};
    fs::create_directories(pic.parent_path());
    pixel3d::save_png(img, pic);
  }
  auto* http = dynamic_cast<HttpProvider*>(&brain);
  std::vector<double> latency;
  auto timed = [&](auto&& f) {
    const auto t0 = GetTickCount64();
    auto r = f();
    latency.push_back(double(GetTickCount64() - t0) / 1000.0);
    return r;
  };
  json results = {{"brain", brain.author()}, {"set1", json::array()}, {"set2", json::array()}, {"set3", json::array()}};

  // Set 1.
  {
    std::ifstream in(eval_dir / "requests.json", std::ios::binary);
    const json set = json::parse(in);
    int ok_n = 0, n = 0;
    for (const auto& r : set["requests"]) {
      ++n;
      const json e = r["expect"];
      Rig rig(out / "set1" / std::to_string(n), brain);
      std::vector<Attachment> att;
      if (!e.value("wants_status", false)) att.push_back({pic, "picture"});
      std::vector<Message> ms;
      std::string error;
      try {
        ms = timed([&] { return rig.session->user(r["text"].get<std::string>(), att); });
      } catch (const std::exception& ex) {
        error = ex.what();
      }
      bool ok = error.empty();
      std::string why = error;
      const ToolCall* spec = first_call(ms, "make_spec");
      if (ok && (e.contains("ask") || e.value("wants_status", false))) {
        if (spec) ok = false, why += " started a spec;";
        const std::string t = reply_text(ms);
        if (e.contains("ask") && t.find('?') == std::string::npos && t.find("\xD8\x9F") == std::string::npos) ok = false, why += " no question;";
      } else if (ok) {
        if (!spec) ok = false, why += " no make_spec;";
        else {
          const json& a = spec->args;
          auto check = [&](const char* k, const char* ak) {
            if (!e.contains(k)) {
              if (a.contains(ak) && !a[ak].is_null() && !(a[ak].is_string() && a[ak].get<std::string>().empty())) ok = false, why += std::string(" ") + ak + " not asked for;";
              return;
            }
            if (!a.contains(ak)) { ok = false; why += std::string(" ") + ak + " missing;"; return; }
            if (std::string(k) == "metres") {
              if (!a[ak].is_number() || std::abs(a[ak].get<double>() - e[k].get<double>()) > 0.01 * e[k].get<double>()) ok = false, why += " metres " + a[ak].dump() + ";";
            } else if (std::string(k) == "style") {
              const std::string got = a[ak].is_string() ? norm(a[ak].get<std::string>()) : std::string();
              std::string want = norm(e[k].get<std::string>()), w;
              for (std::istringstream ss(want); ss >> w;)
                if (got.find(w) == std::string::npos) { ok = false; why += " style " + a[ak].dump() + ";"; break; }
            } else if (a[ak] != e[k] && !(a[ak].is_number() && e[k].is_number() && a[ak].get<double>() == e[k].get<double>())) {
              ok = false;
              why += std::string(" ") + ak + " " + a[ak].dump() + ";";
            }
          };
          check("asset_type", "asset_type");
          check("platform", "platform");
          check("triangle_budget", "triangle_budget");
          check("metres", "metres");
          check("texture_size", "texture_size");
          check("style", "requested_style");
        }
      }
      ok_n += ok;
      std::cout << "set1 " << (ok ? "OK   " : "MISS ") << latency.back() << " s  " << r["text"].get<std::string>() << (ok ? "" : " —" + why) << std::endl;
      results["set1"].push_back({{"text", r["text"]}, {"ok", ok}, {"why", why}, {"seconds", latency.back()}, {"calls", calls_in(ms)},
                                 {"spec", spec ? spec->args : json(nullptr)}, {"reply", reply_text(ms)}});
    }
    results["set1_score"] = {ok_n, n};
    std::cout << "SET1 " << ok_n << " / " << n << std::endl;
  }

  // Set 2.
  if (!set2_dir.empty() && fs::exists(eval_dir / "set2_truth.json")) {
    std::ifstream in(eval_dir / "set2_truth.json", std::ios::binary);
    const json truth = json::parse(in);
    int correct = 0, given = 0, total = 0;
    for (const auto& [name, files] : truth.items()) {
      Rig rig(out / "set2" / name, brain);
      std::vector<Attachment> att;
      for (const auto& [f, side] : files.items()) att.push_back({set2_dir / name / f, "picture"}), ++total;
      std::vector<Message> ms;
      try {
        ms = timed([&] {
          return rig.session->user("These pictures show one character from different sides, in no particular order. Make it game-ready for PC, 1.8 m tall.", att);
        });
      } catch (...) {
      }
      // The brain's labels: propose_labels.labels (path -> side) and the front it chose for make_spec.
      std::map<std::string, std::string> got;
      if (const ToolCall* p = first_call(ms, "propose_labels"))
        for (const auto& [path, side] : p->args.value("labels", json::object()).items())
          got[pixel3d::to_utf8(pixel3d::path_from_utf8(path).filename())] = side.is_string() ? side.get<std::string>() : std::string();
      json item = {{"set", name}, {"labels", got}, {"truth", files}, {"seconds", latency.back()}, {"calls", calls_in(ms)}, {"reply", reply_text(ms)}};
      int c = 0, g = 0;
      for (const auto& [f, side] : got) {
        if (side.empty() || side == "auto") continue;
        ++g;
        c += files.contains(f) && files[f] == side;
      }
      correct += c, given += g;
      std::cout << "set2 " << name << ": " << c << " correct of " << g << " labelled (" << files.size() << " pictures), " << latency.back() << " s" << std::endl;
      results["set2"].push_back(item);
    }
    results["set2_score"] = {{"correct", correct}, {"labelled", given}, {"pictures", total}};
    std::cout << "SET2 " << correct << " correct / " << given << " labelled / " << total << " pictures" << std::endl;
  }

  // Set 3.
  {
    std::ifstream in(eval_dir / "conversations.json", std::ios::binary);
    const json set = json::parse(in);
    int ok_n = 0, n = 0;
    for (const auto& conv : set["conversations"]) {
      ++n;
      Rig rig(out / "set3" / std::to_string(n), brain);
      bool ok = true;
      json turns = json::array();
      for (const auto& t : conv["turns"]) {
        std::vector<Message> ms;
        std::string error;
        try {
          if (t.contains("user")) {
            std::vector<Attachment> att;
            for (int i = 0; i < t.value("attach", 0); ++i) att.push_back({pic, "picture"});
            ms = timed([&] { return rig.session->user(t["user"].get<std::string>(), att); });
          } else {
            if (rig.started.empty()) throw std::runtime_error("no run was started for the fixture");
            write_fixture(rig.started.back(), t["fixture"].get<std::string>());
            ms = timed([&] { return rig.session->event(read_run_state(rig.started.back())); });
          }
        } catch (const std::exception& ex) {
          error = ex.what();
        }
        const auto calls = calls_in(ms);
        bool tok = error.empty();
        for (const auto& e : t.value("expect", json::array()))
          if (std::find(calls.begin(), calls.end(), e.get<std::string>()) == calls.end()) tok = false;
        for (const auto& f : t.value("forbidden", json::array()))
          if (std::find(calls.begin(), calls.end(), f.get<std::string>()) != calls.end()) tok = false;
        ok = ok && tok;
        turns.push_back({{"turn", t}, {"calls", calls}, {"ok", tok}, {"error", error}, {"reply", reply_text(ms)}});
      }
      ok_n += ok;
      std::cout << "set3 " << (ok ? "OK   " : "MISS ") << conv["name"].get<std::string>() << std::endl;
      results["set3"].push_back({{"name", conv["name"]}, {"ok", ok}, {"turns", turns}});
    }
    results["set3_score"] = {ok_n, n};
    std::cout << "SET3 " << ok_n << " / " << n << std::endl;
  }
  std::vector<double> l = latency;
  std::sort(l.begin(), l.end());
  results["latency_seconds"] = {{"median", l.empty() ? 0.0 : l[l.size() / 2]}, {"max", l.empty() ? 0.0 : l.back()}, {"turns", l.size()}};
  std::cout << "LATENCY median " << (l.empty() ? 0.0 : l[l.size() / 2]) << " s, max " << (l.empty() ? 0.0 : l.back()) << " s over " << l.size()
            << " turns" << std::endl;
  (void)http;
  std::ofstream o(out / "results.json", std::ios::binary);
  o << results.dump(1, ' ', false, json::error_handler_t::replace);
  return 0;
}

}  // namespace p3dchat
