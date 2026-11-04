// The rules brain: a fixed procedure, no model. It reads the conversation, asks for the first missing required
// field, then calls propose_labels → make_spec → validate_spec → run_agent, and describes run states from the
// agent's own files. Everything it says about results is quoted from the report and marked as measured.
#include <sstream>

#include "chat.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dchat {

namespace {

struct Lang {
  bool ar = false;
  std::string t(const std::string& en, const std::string& arabic) const { return ar ? arabic : en; }
};

std::string fmt_num(double v, int digits) {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(digits);
  o << v;
  return o.str();
}

// The latest run this conversation knows (from run_agent results and events).
std::string current_run(const std::vector<Message>& c) {
  for (auto it = c.rbegin(); it != c.rend(); ++it)
    if ((it->role == "tool" || it->role == "event") && it->result.is_object() && it->result.contains("run") && it->result.value("ok", true) &&
        it->result["run"].is_string())
      return it->result["run"].get<std::string>();
  return {};
}
// The latest known state of that run (a run_status / open_report result or an event).
json last_state(const std::vector<Message>& c) {
  for (auto it = c.rbegin(); it != c.rend(); ++it)
    if ((it->role == "tool" || it->role == "event") && it->result.is_object() && it->result.contains("state")) return it->result;
  return nullptr;
}
const Message* last_user(const std::vector<Message>& c) {
  for (auto it = c.rbegin(); it != c.rend(); ++it)
    if (it->role == "user") return &*it;
  return nullptr;
}
// The draft request: every user message since the last run was started (a new request starts after it).
Draft draft_of(const std::vector<Message>& c) {
  size_t from = 0;
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i].role == "tool" && c[i].tool == "run_agent" && c[i].result.value("ok", false)) from = i + 1;
  Draft d;
  for (size_t i = from; i < c.size(); ++i) {
    if (c[i].role != "user") continue;
    read_words(c[i].text, d);
    for (const auto& a : c[i].attachments) {
      if (a.kind == "picture") d.pictures.push_back(a);
      else if (a.kind == "glb") d.glbs.push_back(a);
    }
  }
  return d;
}
const Message* last_tool(const std::vector<Message>& c, const std::string& name) {
  for (auto it = c.rbegin(); it != c.rend(); ++it) {
    if (it->role == "user") return nullptr;
    if (it->role == "tool" && it->tool == name) return &*it;
  }
  return nullptr;
}

ToolCall call(const std::string& name, json args) { return {"", name, std::move(args)}; }

std::string capability_rigging(const Lang& L) {
  return L.t("3re does not rig generated characters: there is no automatic skeleton or skinning for TRELLIS models (Rigging works "
             "only for Pixel Art Builder models). I can deliver the model game-ready, textured and checked in Godot; rig it in "
             "Blender or with an auto-rigger afterwards.",
             "3re لا يضيف هيكلاً عظمياً (Rigging) للشخصيات المولَّدة: لا يوجد تسليح أو أوزان تلقائية لنماذج TRELLIS (صفحة Rigging تعمل لنماذج "
             "Pixel Art Builder فقط). أستطيع تسليمك النموذج جاهزاً للعبة، بنسيجه، ومفحوصاً في Godot؛ والتسليح يكون بعدها في Blender أو بأداة تسليح تلقائي.");
}

std::string style_note(const Lang& L, const std::string& style) {
  return L.t("Requested style \"" + style + "\" is recorded in the spec and the report, but NOT applied: style transfer is not "
             "implemented yet. The texture comes from your pictures.",
             "الأسلوب المطلوب \"" + style + "\" سُجِّل في المواصفة والتقرير لكنه لن يُطبَّق: نقل الأسلوب غير موجود بعد. النسيج يأتي من صورك.");
}

// A run state in words. Status, criteria and numbers are copied from the agent's files ("measured").
std::string describe(const Lang& L, const json& s) {
  std::ostringstream o;
  const std::string state = s.value("state", std::string());
  if (state == "running") {
    o << L.t("The agent is working", "الوكيل يعمل الآن");
    if (!s.value("running_tool", std::string()).empty()) o << L.t(": running ", ": يشغّل ") << s["running_tool"].get<std::string>();
    o << ".";
    return o.str();
  }
  if (state == "waiting") {
    const auto& q = s["question"];
    o << L.t("The agent needs your decision (", "الوكيل يحتاج قرارك (") << q.value("id", "") << "): " << q.value("title", "") << "\n";
    if (!q.value("why", std::string()).empty()) o << L.t("Why (measured): ", "السبب (مقاس): ") << q.value("why", "") << "\n";
    o << L.t("Options:", "الخيارات:") << "\n";
    for (const auto& opt : q.value("options", json::array())) {
      o << "• " << opt.value("id", "") << " — " << opt.value("label", "");
      if (opt.value("needs_picture", false)) o << L.t(" (attach the picture with \"+\" and say which side)", " (أرفق الصورة بزر \"+\" وقل أي جهة)");
      o << "\n";
    }
    const auto opts = q.value("options", json::array());
    if (!opts.empty()) o << L.t("Reply with the option, e.g. \"", "اكتب الخيار، مثلاً \"") << opts[0].value("id", "") << "\".";
    return o.str();
  }
  if (state == "finished") {
    o << L.t("Result (measured by the agent): ", "النتيجة (مقاسة من الوكيل): ") << s.value("status", "") << " — " << s.value("summary", "") << "\n";
    bool all_pass = s.value("status", std::string()) == "PASS" || s.value("status", std::string()) == "ACCEPTED";
    std::map<std::string, std::string> st;
    for (const auto& c : s.value("acceptance", json::array())) {
      o << "• " << c.value("criterion", "") << ": " << c.value("status", "") << " — " << c.value("detail", "") << "\n";
      st[c.value("criterion", "")] = c.value("status", "");
    }
    int accepted = 0, rejected = 0;
    for (const auto& p : s.value("pictures", json::array())) {
      (p.value("status", std::string()) == "accepted" ? accepted : rejected)++;
      o << "• " << L.t("picture ", "صورة ") << p.value("name", "") << " (" << p.value("label", "") << "): " << p.value("status", "");
      if (!p.value("reason", std::string()).empty()) o << " — " << p.value("reason", "");
      o << "\n";
    }
    for (const auto& d : s.value("deliverables", json::array()))
      o << L.t("Deliverable: ", "الملف الناتج: ") << d.value("path", "") << " (SHA-256 " << d.value("sha256", "").substr(0, 12) << "…)\n";
    if (s.contains("requested_style")) o << style_note(L, s["requested_style"].value("style", "")) << "\n";
    // "Polished" is a measurement, not an opinion: sides from real pictures, budget, fidelity, texture, Godot.
    const bool polished = all_pass && accepted > 0 && rejected == 0 && st["triangle_budget"] == "PASS" && st["fidelity"] == "PASS" &&
                          st["texture"] == "PASS" && st["godot_import"] == "PASS";
    o << L.t("Polished by the measured definition (sides painted from real pictures, budget, fidelity, texture and Godot all PASS): ",
             "مصقول حسب التعريف المقاس (جوانب من صور حقيقية، الميزانية، الدقة، النسيج و Godot كلها PASS): ")
      << (polished ? L.t("yes", "نعم") : L.t("no", "لا"));
    if (!polished && accepted == 0) o << L.t(" — no side was painted from a real picture", " — لم تُلوَّن أي جهة من صورة حقيقية");
    o << ".";
    // A rejected picture: the registration disagreed with its label — the person decides which side it shows.
    for (const auto& p : s.value("pictures", json::array()))
      if (p.value("status", std::string()) == "rejected")
        o << "\n"
          << L.t("Picture " + p.value("name", std::string()) + " did not fit the model as \"" + p.value("label", std::string()) +
                     "\". Which side does it show (back, left, right, back-left, …)? Tell me and I paint again with it; or attach a "
                     "better picture of that side.",
                 "الصورة " + p.value("name", std::string()) + " لم تطابق النموذج كجهة \"" + p.value("label", std::string()) +
                     "\". أي جهة تُظهر (خلف، يسار، يمين، خلف-يسار…)؟ أخبرني وأعيد التلوين بها؛ أو أرفق صورة أفضل لتلك الجهة.");
    return o.str();
  }
  return L.t("The run has not started.", "التشغيل لم يبدأ.");
}

// Which option of the open question the words choose ("" = none).
std::string chosen_option(const std::string& text, const json& question, bool has_picture) {
  std::string t;
  for (char c : text) t += static_cast<unsigned char>(c) < 128 ? char(std::tolower(static_cast<unsigned char>(c))) : c;
  for (const auto& o : question.value("options", json::array()))
    if (t.find(o.value("id", std::string())) != std::string::npos && (!o.value("needs_picture", false) || has_picture))
      return o.value("id", std::string());
  const std::string kind = question.value("kind", std::string());
  auto any = [&](std::initializer_list<const char*> ws) {
    for (const char* w : ws)
      if (t.find(w) != std::string::npos) return true;
    return false;
  };
  if (kind == "budget_or_fidelity") {
    if (any({"more triangles", "use more", "need", "أكثر", "المطلوب", "زيد", "زود"})) return "needed";
    if (any({"keep the budget", "stay", "الميزانية", "بالميزانية", "التزم"})) return "budget";
  }
  if (kind == "hidden_side_picture") {
    if (has_picture) return "picture";
    if (any({"keep", "as is", "leave", "خليه", "اتركه", "كما هو", "ابقه"})) return "keep";
  }
  return {};
}

// The side words name ("" = none): back, left, right, three-quarters, in English and Arabic.
std::string side_words(const std::string& text) {
  std::string t;
  for (char c : text) t += static_cast<unsigned char>(c) < 128 ? char(std::tolower(static_cast<unsigned char>(c))) : c;
  auto has = [&](std::initializer_list<const char*> ws) {
    for (const char* w : ws)
      if (t.find(w) != std::string::npos) return true;
    return false;
  };
  const bool back = has({"back", "rear", "behind", "خلف", "ظهر", "الخلف"}), front = has({"front", "أمام", "امام"});
  const bool left = has({"left", "يسار"}), right = has({"right", "يمين"});
  if (back && left) return "back_left";
  if (back && right) return "back_right";
  if (front && left) return "front_left";
  if (front && right) return "front_right";
  if (back) return "back";
  if (left) return "left";
  if (right) return "right";
  return {};
}

std::string side_label(const std::string& text) {
  std::string t;
  for (char c : text) t += static_cast<unsigned char>(c) < 128 ? char(std::tolower(static_cast<unsigned char>(c))) : c;
  const bool back = t.find("back") != std::string::npos || t.find("خلف") != std::string::npos || t.find("ظهر") != std::string::npos;
  const bool left = t.find("left") != std::string::npos || t.find("يسار") != std::string::npos;
  const bool right = t.find("right") != std::string::npos || t.find("يمين") != std::string::npos;
  if (back && left) return "back_left";
  if (back && right) return "back_right";
  if (left) return "left";
  if (right) return "right";
  return "back";
}

}  // namespace

Reply RulesProvider::next(const std::vector<Message>& c, const json&) {
  Reply r;
  if (c.empty()) return r;
  const Message* u = last_user(c);
  Lang L{u && has_arabic(u->text)};
  const Message& last = c.back();
  const std::string run = current_run(c);
  const json state = last_state(c);
  const Draft d = draft_of(c);

  if (last.role == "event") {
    r.text = describe(L, last.result);
    return r;
  }

  if (last.role == "tool") {
    const json& res = last.result;
    const bool ok = res.value("ok", false);
    if (last.tool == "propose_labels") {
      if (!ok) {
        r.text = L.t("I could not read the pictures: ", "لم أستطع قراءة الصور: ") + res.value("errors", json::array()).dump();
        return r;
      }
      if (res.value("needs_front", false)) {
        r.text = L.t("Which picture is the front? (The 3D shape is built from the front picture; the others only paint sides.) "
                     "Give its file name, or rename it with \"front\".",
                     "أي صورة هي الأمامية؟ (الشكل ثلاثي الأبعاد يُبنى من الصورة الأمامية؛ والبقية تلوّن الجوانب فقط). اكتب اسم ملفها أو أعد تسميتها بكلمة front.");
        return r;
      }
      json args = d.to_json();
      args.erase("pictures");
      args["picture"] = res.value("front", std::string());
      args["original_picture"] = res.value("front_original", std::string());
      args["views"] = res.value("views", json::array());
      r.calls.push_back(call("make_spec", args));
      std::ostringstream o;
      o << L.t("Front: ", "الأمامية: ") << pixel3d::to_utf8(pixel3d::path_from_utf8(res.value("front", std::string())).filename()) << ". ";
      for (const auto& s : res.value("sheets", json::array()))
        o << L.t("Turnaround sheet split into ", "لوحة الدوران قُسِّمت إلى ") << s["parts"].size() << L.t(" pictures. ", " صور. ");
      for (const auto& v : res.value("views", json::array()))
        o << pixel3d::to_utf8(pixel3d::path_from_utf8(v.value("picture", std::string())).filename()) << " → " << v.value("label", "") << " ("
          << L.t("proposed from: ", "مقترح من: ") << v.value("source", "") << "). ";
      o << L.t("These are proposals: the registration on the generated mesh will verify each label and reject wrong or mirrored ones.",
               "هذه اقتراحات: التسجيل على المجسم المولَّد سيتحقق من كل تسمية ويرفض الخاطئة أو المعكوسة.");
      r.text = o.str();
      return r;
    }
    if (last.tool == "make_spec") {
      if (!ok) r.text = L.t("The spec could not be written: ", "تعذّر كتابة المواصفة: ") + res.value("error", std::string());
      else {
        r.calls.push_back(call("validate_spec", {{"spec", res["spec"]}}));
        if (res.contains("reused_generation") && res["reused_generation"].is_object())
          r.text = L.t("This exact picture was already generated in your project ", "هذه الصورة نفسها وُلِّدت سابقاً في مشروعك ") +
                   pixel3d::to_utf8(pixel3d::path_from_utf8(res["reused_generation"].value("project", std::string())).filename()) +
                   L.t(" (same SHA-256): the agent reuses that generation instead of running TRELLIS again.",
                       " (نفس SHA-256): الوكيل يعيد استخدام ذلك التوليد بدل تشغيل TRELLIS من جديد.");
      }
      return r;
    }
    if (last.tool == "validate_spec") {
      if (!ok) r.text = L.t("The agent's validator refused the spec: ", "مدقّق الوكيل رفض المواصفة: ") + res.value("errors", json::array()).dump();
      else r.calls.push_back(call("run_agent", {{"spec", res["spec"]}}));
      return r;
    }
    if (last.tool == "run_agent") {
      if (!ok && res.value("godot_missing", false)) {
        r.text = L.t("Not started: Godot 4 was not found on this computer, so the delivery cannot be imported and checked and no model can "
                     "PASS. Install Godot 4 (godotengine.org) or choose its console executable in Settings > Delivery check (Godot), then "
                     "say \"start\".",
                     "لم يبدأ: لم أجد Godot 4 على هذا الجهاز، فلا يمكن استيراد التسليم وفحصه ولن يصل أي نموذج إلى PASS. ثبّت Godot 4 "
                     "(godotengine.org) أو اختر ملفه التنفيذي console من الإعدادات ← فحص التسليم (Godot)، ثم قل \"ابدأ\".");
        if (!res.value("problem", std::string()).empty()) r.text += "\n" + res["problem"].get<std::string>();
        return r;
      }
      if (!ok && res.value("waiting", false)) {
        r.text = L.t("Not started yet — the machine is not ready (measured): ", "لم يبدأ بعد — الجهاز غير جاهز (مقاس): ") + res.value("reason", std::string()) +
                 L.t(". Say \"start\" when you want me to check again.", ". قل \"ابدأ\" عندما تريدني أن أتحقق مجدداً.");
        return r;
      }
      if (!ok) {
        r.text = L.t("The agent did not start: ", "الوكيل لم يبدأ: ") + res.value("error", std::string()) + res.value("errors", json::array()).dump();
        return r;
      }
      const auto* spec = last_tool(c, "validate_spec");
      std::ostringstream o;
      o << L.t("Plan: the agent will prepare the model (generate or reuse), check its geometry, reduce it to the budget",
               "الخطة: سيجهّز الوكيل النموذج (توليد أو إعادة استخدام)، يفحص الهندسة، يخفّضه للميزانية")
        << (spec ? " (" + std::to_string(spec->result.value("triangle_budget", 0)) + L.t(" triangles", " مثلث") + ")" : std::string())
        << L.t(", texture it from your pictures, check fidelity and texture, then import it in Godot. It started in the background; "
               "every step appears in the run viewer, and if it cannot decide alone it will ask you here.",
               "، يلوّنه من صورك، يفحص الدقة والنسيج، ثم يستورده في Godot. بدأ في الخلفية؛ كل خطوة تظهر في عارض التشغيل، وإن لم يستطع القرار وحده سيسألك هنا.");
      // The request as written into the spec (the draft restarts once a run has started).
      const auto* made = last_tool(c, "make_spec");
      const std::string style = made ? made->result.value("content", json::object()).value("requested_style", std::string()) : std::string();
      if (!style.empty()) o << "\n" << style_note(L, style);
      Draft asked;
      if (u) read_words(u->text, asked);
      if (!asked.engine_words.empty())
        o << "\n" << L.t("Note: delivery is checked in Godot only; the GLB also opens in Unity/Unreal, but their import is not verified by 3re.",
                         "ملاحظة: التسليم يُفحص في Godot فقط؛ ملف GLB يفتح في Unity/Unreal أيضاً لكن 3re لا يتحقق من استيرادهما.");
      if (asked.wants_rigging) o << "\n" << capability_rigging(L);
      if (const auto g = res.value("godot", json::object()); !g.value("version", std::string()).empty())
        o << "\n" << L.t("Delivery check: Godot ", "فحص التسليم: Godot ") << g.value("version", "") << " — " << g.value("exe", "");
      r.text = o.str();
      return r;
    }
    if (last.tool == "retexture_with_pictures") {
      if (!ok && res.value("waiting", false))
        r.text = L.t("Not started yet — the machine is not ready (measured): ", "لم يبدأ بعد — الجهاز غير جاهز (مقاس): ") + res.value("reason", std::string());
      else if (!ok)
        r.text = L.t("Could not paint again: ", "تعذّرت إعادة التلوين: ") + res.value("error", std::string()) + res.value("errors", json::array()).dump();
      else {
        std::ostringstream o;
        o << L.t("Painting the same model again with ", "أعيد تلوين النموذج نفسه بـ ") << res.value("added_pictures", json::array()).size()
          << L.t(" more picture(s) (", " صورة إضافية (");
        for (const auto& p : res.value("added_pictures", json::array()))
          o << pixel3d::to_utf8(pixel3d::path_from_utf8(p.value("original", std::string())).filename()) << " → " << p.value("label", "") << "; ";
        o << L.t("). Same generation, budget and size; the registration verifies each picture. I will show the measured result here.",
                 "). نفس التوليد والميزانية والحجم؛ التسجيل يتحقق من كل صورة. سأعرض النتيجة المقاسة هنا.");
        r.text = o.str();
      }
      return r;
    }
    if (last.tool == "answer_decision") {
      r.text = ok ? L.t("Answer sent (", "أُرسل الجواب (") + res.value("choice", std::string()) + ", " + res.value("who", std::string()) +
                        L.t("). The agent continues; I will show what it measures.", "). الوكيل يكمل؛ سأعرض ما يقيسه.")
                  : L.t("The answer was refused: ", "رُفض الجواب: ") + res.value("error", std::string());
      return r;
    }
    if (last.tool == "region_views") {
      if (!ok) {
        r.text = res.value("error", std::string());
        return r;
      }
      std::ostringstream o;
      o << L.t("Mark the problem on one of these views (they use the same framing as the fix): ", "حدّد المشكلة على إحدى هذه الصور (بنفس إطار الإصلاح): ");
      for (const auto& v : res.value("views", json::array())) o << "\n• yaw " << v.value("yaw", 0.0) << ": " << v.value("png", "");
      o << L.t("\nIn the app: \"Mark a problem…\". Typed: \"mark yaw=0 x=235 y=117 r=7\" and what is wrong.",
               "\nفي التطبيق: \"تحديد مشكلة…\". أو اكتب: \"mark yaw=0 x=235 y=117 r=7\" مع وصف المشكلة.");
      r.text = o.str();
      return r;
    }
    if (last.tool == "regional_fix") {
      if (!ok) {
        r.text = L.t("The regional fix could not run: ", "تعذّر تشغيل الإصلاح الموضعي: ") + res.value("error", std::string());
        return r;
      }
      std::ostringstream o;
      const auto reg = res.value("region", json::object());
      o << L.t("Measured in the marked region (", "المقاس في المنطقة المحددة (") << reg.value("faces", 0) << L.t(" faces, ", " وجه، ") << reg.value("texels", 0)
        << L.t(" texels):", " تكسل):") << "\n";
      const auto& pb = res.value("painted_by", json::object());
      if (pb.value("available", false)) {
        o << L.t("• painted by: ", "• لُوِّنت من: ");
        for (auto& [k, v] : pb["share"].items()) o << k << " " << int(v.get<double>() * 100 + 0.5) << "%; ";
        o << "\n";
      }
      const bool sees = res.value("source_sees_region", false);
      o << L.t("• the source picture sees this region: ", "• الصورة المصدر ترى هذه المنطقة: ") << (sees ? L.t("yes", "نعم") : L.t("no", "لا"));
      if (sees) o << L.t(", colour distance to it now ", "، بُعد اللون عنها الآن ") << fmt_num(res.value("before_picture_distance", 0.0), 4);
      o << "\n";
      int k = 0;
      for (const auto& a : res.value("attempts", json::array())) {
        ++k;
        o << L.t("• attempt ", "• محاولة ") << k << " (" << pixel3d::to_utf8(pixel3d::path_from_utf8(a.value("candidate", std::string())).filename()) << "): ";
        if (a.value("status", std::string()) != "made") {
          o << L.t("skipped — ", "تُركت — ") << a.value("reason", "") << "\n";
          continue;
        }
        if (a.contains("picture_distance")) o << L.t("distance ", "البُعد ") << fmt_num(a["picture_distance"].get<double>(), 4) << ", ";
        o << L.t("gate ", "الفحص ") << a.value("gate", std::string("-")) << L.t(", faces outside the region that changed ", "، أوجه خارج المنطقة تغيّرت ")
          << a.value("faces_outside_changed", -1) << L.t(" (bordering faces touched by filtering: ", " (أوجه ملاصقة تأثرت بالتنعيم: ")
          << a.value("faces_on_region_edge_changed", -1) << L.t("), view pixels changed outside ", ")، بكسلات تغيّرت خارجها في العرض ")
          << a.value("view_pixels_changed_outside_region", -1) << "\n";
      }
      if (res.value("closest_to_picture", json()).is_object()) {
        const auto& b = res["closest_to_picture"];
        o << L.t("Closest to your picture: ", "الأقرب لصورتك: ") << pixel3d::to_utf8(pixel3d::path_from_utf8(b.value("candidate", std::string())).filename())
          << L.t(". Compare the close-ups: ", ". قارن الصور المقرّبة: ") << b.value("closeup", "")
          << L.t(". If you keep it, the fixed model is ", ". إن أردت الاحتفاظ به فالنموذج المصلَّح هو ") << b.value("glb", "")
          << L.t(" — its texture gate is measured; the delivery checks (front, scale, Godot) are not re-run for it yet: UNVERIFIED.",
                 " — فحص نسيجه مقاس؛ أما فحوص التسليم (الواجهة، المقياس، Godot) فلم تُعَد له بعد: UNVERIFIED.");
      } else {
        o << L.t("No fix to propose: ", "لا إصلاح أقترحه: ") << res.value("limit", "");
        int made = 0;
        for (const auto& a : res.value("attempts", json::array())) made += a.value("status", std::string()) == "made";
        if (!sees) o << L.t(". What would fix it: a picture that shows this side (attach it and I re-texture with it).",
                            ". ما يصلحه: صورة تُظهر هذه الجهة (أرفقها وأعيد التلوين بها).");
        else if (made > 0)
          o << L.t(". None of the attempts is closer to your picture here than the current texture; if it still looks wrong, the picture "
                   "itself shows it so.",
                   ". لا محاولة أقرب لصورتك هنا من النسيج الحالي؛ إن بدت خاطئة فالصورة نفسها تُظهرها هكذا.");
        else o << L.t(".", ".");
      }
      r.text = o.str();
      return r;
    }
    if (last.tool == "run_status" || last.tool == "open_report") {
      r.text = ok ? describe(L, res) : res.value("error", std::string());
      return r;
    }
    r.text = res.dump();
    return r;
  }

  // A person spoke.
  if (!u) return r;
  Draft now;
  read_words(u->text, now);
  bool picture_now = false;
  for (const auto& a : u->attachments) picture_now |= a.kind == "picture";
  for (const auto& a : u->attachments)
    if (a.kind == "unsupported") {
      r.text = L.t("I can use pictures (PNG, JPG, WebP, Aseprite, PSD) and GLB models; this file is neither: ",
                   "أستطيع استخدام الصور (PNG, JPG, WebP, Aseprite, PSD) ونماذج GLB؛ هذا الملف ليس منها: ") +
               pixel3d::to_utf8(a.path.filename());
      return r;
    }

  // An answer to the agent's open question.
  if (!run.empty() && state.is_object() && state.value("state", std::string()) == "waiting") {
    const json& q = state["question"];
    const std::string choice = chosen_option(u->text, q, picture_now);
    if (!choice.empty()) {
      json args = {{"run", run}, {"question", q.value("id", "")}, {"choice", choice}};
      if (choice == "picture") {
        for (const auto& a : u->attachments)
          if (a.kind == "picture") args["picture"] = pixel3d::to_utf8(a.path);
        args["label"] = side_label(u->text);
      }
      r.calls.push_back(call("answer_decision", args));
      return r;
    }
  }
  // W5: a run that waited for the machine starts when the person says so (measured again).
  if (const auto* wait = [&]() -> const Message* {
        for (auto it = c.rbegin(); it != c.rend(); ++it)
          if (it->role == "tool" && it->tool == "run_agent") return it->result.value("waiting", false) ? &*it : nullptr;
        return nullptr;
      }()) {
    std::string t;
    for (char ch : u->text) t += static_cast<unsigned char>(ch) < 128 ? char(std::tolower(static_cast<unsigned char>(ch))) : ch;
    const bool new_request = !u->attachments.empty() || now.asset_type || now.platform || now.metres;
    if (!new_request && (t.find("start") != std::string::npos || t.find("retry") != std::string::npos || t.find("ابدأ") != std::string::npos ||
                         t.find("ابدا") != std::string::npos || t.find("جرب") != std::string::npos)) {
      r.calls.push_back(call("run_agent", {{"spec", wait->result.value("spec", std::string())}}));
      return r;
    }
  }
  // W4: a problem in the finished model.
  const bool finished = state.is_object() && state.value("state", std::string()) == "finished";
  // More pictures for a finished model (no new request words): paint it again with them.
  if (!run.empty() && finished && picture_now && !now.platform && !now.triangle_budget && !now.metres) {  // a type word alone is no new request
    json pics = json::array();
    const std::string said = side_words(u->text);
    for (const auto& a : u->attachments)
      if (a.kind == "picture") pics.push_back({{"path", pixel3d::to_utf8(a.path)}, {"label", said.empty() ? std::string("auto") : said}});
    r.calls.push_back(call("retexture_with_pictures", {{"run", run}, {"pictures", pics}}));
    return r;
  }
  // The side of a picture the registration rejected, named by the person.
  if (!run.empty() && finished && !picture_now && !side_words(u->text).empty()) {
    json pics = json::array();
    for (const auto& p : state.value("pictures", json::array()))
      if (p.value("status", std::string()) == "rejected") {
        // view-N-<label>.png = the N-th extra picture of the spec: its label is corrected, not duplicated.
        const std::string n = p.value("name", std::string());
        int index = 0;
        if (n.rfind("view-", 0) == 0) index = std::atoi(n.c_str() + 5);
        if (index > 0) pics.push_back({{"replace", index}, {"label", side_words(u->text)}});
      }
    if (pics.size() == 1) {  // more than one rejected picture: which one is meant is not clear from words alone
      r.calls.push_back(call("retexture_with_pictures", {{"run", run}, {"pictures", pics}}));
      return r;
    }
  }
  if (!run.empty() && finished && now.mark) {
    json args = *now.mark;
    args["run"] = run;
    args["note"] = u->text;
    r.calls.push_back(call("regional_fix", args));
    return r;
  }
  {
    std::string t;
    for (char ch : u->text) t += static_cast<unsigned char>(ch) < 128 ? char(std::tolower(static_cast<unsigned char>(ch))) : ch;
    const bool problem = t.find("problem") != std::string::npos || t.find("wrong") != std::string::npos || t.find("smear") != std::string::npos ||
                         t.find("مشكلة") != std::string::npos || t.find("خطأ") != std::string::npos || t.find("غلط") != std::string::npos;
    if (!run.empty() && finished && problem && u->attachments.empty()) {
      r.calls.push_back(call("region_views", {{"run", run}}));
      return r;
    }
  }
  if (now.wants_rigging && u->attachments.empty()) {
    r.text = capability_rigging(L);
    return r;
  }
  if (!run.empty() && (now.wants_status || now.wants_report) && !now.asset_type && u->attachments.empty()) {
    r.calls.push_back(call(now.wants_report ? "open_report" : "run_status", {{"run", run}}));
    return r;
  }
  if (!run.empty() && state.is_object() && state.value("state", std::string()) != "finished" && !now.asset_type && u->attachments.empty()) {
    r.calls.push_back(call("run_status", {{"run", run}}));
    return r;
  }

  // A production request: ask for the first missing required field, in this order.
  if (d.pictures.empty() && d.glbs.empty()) {
    r.text = L.t("Attach the picture (or pictures, any order) or a GLB with \"+\" or drag them here.",
                 "أرفق الصورة (أو الصور، بأي ترتيب) أو ملف GLB بزر \"+\" أو اسحبها إلى هنا.");
    return r;
  }
  if (!d.asset_type) {
    r.text = L.t("Is it a character or a prop (weapon, furniture, vehicle, object)?", "هل هي شخصية أم عنصر (سلاح، أثاث، مركبة، غرض)؟");
    return r;
  }
  if (!d.platform && !d.triangle_budget) {
    r.text = L.t("Which target: mobile, PC/console, or a high-detail hero asset? (Or give a triangle budget, e.g. \"8000 triangles\".)",
                 "ما المنصة: موبايل، كمبيوتر/كونسول، أم أصل بطولي عالي التفاصيل؟ (أو حدّد ميزانية مثلثات، مثلاً \"8000 مثلث\").");
    return r;
  }
  if (!d.metres) {
    r.text = *d.asset_type == "character" ? L.t("How tall is the character in metres (e.g. 1.8 m)?", "كم طول الشخصية بالأمتار (مثلاً 1.8 متر)؟")
                                          : L.t("How long is it in metres (its longest side, e.g. 1.1 m)?", "كم طوله بالأمتار (أطول جهة، مثلاً 1.1 متر)؟");
    return r;
  }
  if (!d.pictures.empty()) {
    json pics = json::array(), labels = json::object();
    for (const auto& p : d.pictures) {
      pics.push_back(pixel3d::to_utf8(p.path));
      // "the front is hero_a.png": a file name in the person's words makes that picture the front.
      for (size_t i = 0; i < c.size(); ++i)
        if (c[i].role == "user" && c[i].text.find(pixel3d::to_utf8(p.path.filename())) != std::string::npos)
          labels[pixel3d::to_utf8(p.path)] = "front";
    }
    json args = {{"pictures", pics}};
    if (!labels.empty()) args["labels"] = labels;
    r.calls.push_back(call("propose_labels", args));
    return r;
  }
  r.calls.push_back(call("make_spec", d.to_json()));
  return r;
}

Reply FakeProvider::next(const std::vector<Message>&, const json&) {
  if (script_.empty()) return {"(fake provider: no scripted reply left)", {}};
  Reply r = std::move(script_.front());
  script_.pop_front();
  return r;
}

}  // namespace p3dchat
