// The chat's tools: everything the brain can make happen goes through here, with real effects (files written,
// the agent started, answers written) and results read from the agent's own files.
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "agent/agent_decide.hpp"
#include "agent/agent_eval.hpp"
#include "agent/agent_exec.hpp"
#include "agent/agent_spec.hpp"
#include "chat.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/common/sha256.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dchat {

namespace {

std::string u8(const fs::path& p) { return pixel3d::to_utf8(p); }
fs::path from_u8(const std::string& s) { return pixel3d::path_from_utf8(s); }

json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

std::string stamp() {
  const auto now = std::chrono::system_clock::now();
  const auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  char m[8];
  std::snprintf(m, sizeof m, "-%03d", int(ms));
  return std::string(b) + m;
}

std::string safe_name(std::string s) {
  std::string out;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    out += (u < 32 || std::string("/\\:*?\"<>|").find(c) != std::string::npos) ? '_' : c;
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  while (!out.empty() && (out.front() == '-' || out.front() == '.')) out.erase(out.begin());
  if (out.size() > 60) {
    size_t n = 60;
    while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 sequence
    out.resize(n);
  }
  return out.empty() ? std::string("asset") : out;
}

json read_json(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  if (!in) return nullptr;
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    return json::parse(ss.str());
  } catch (...) {
    return nullptr;
  }
}

std::string lower(std::string s) {
  for (auto& c : s)
    if (static_cast<unsigned char>(c) < 128) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The side a file name says (English and Arabic words), "" when it says none.
std::string label_from_name(const fs::path& p) {
  const std::string n = lower(u8(p.stem()));
  auto has = [&](std::initializer_list<const char*> ws) {
    for (const char* w : ws)
      if (n.find(w) != std::string::npos) return true;
    return false;
  };
  const bool back = has({"back", "rear", "خلف", "ظهر", "الخلف"});
  const bool front = has({"front", "أمام", "امام", "الأمام", "وجه"});
  const bool left = has({"left", "يسار", "اليسار"});
  const bool right = has({"right", "يمين", "اليمين"});
  const bool three_quarter = has({"3-4", "3_4", "34", "three", "quarter", "ثلاثة أرباع"});
  if (back && left) return "back_left";
  if (back && right) return "back_right";
  if (front && left) return "front_left";
  if (front && right) return "front_right";
  if (back) return "back";
  if (front && three_quarter) return "auto";
  if (front) return "front";
  if (left) return "left";
  if (right) return "right";
  if (has({"side", "جانب", "الجانب"}) || three_quarter) return "auto";  // which side: the registration decides
  return "";
}

// Copies a picture into the conversation as PNG (the agent and the texture worker read PNG; Aseprite / PSD / WebP
// are flattened by pixel3d::decode_image). The person's file is never written.
std::optional<fs::path> to_png(const fs::path& src, const fs::path& dir, std::string& error) {
  auto bytes = pixel3d::read_file(src, 256u << 20);
  if (!bytes) {
    error = "cannot read " + u8(src);
    return std::nullopt;
  }
  auto img = pixel3d::decode_image(*bytes, u8(src.filename()));
  if (!img) {
    error = u8(src.filename()) + " is not a picture 3re can read";
    return std::nullopt;
  }
  fs::create_directories(dir);
  fs::path out = dir / (safe_name(u8(src.stem())) + ".png");
  for (int n = 2; fs::exists(out); ++n) out = dir / (safe_name(u8(src.stem())) + " (" + std::to_string(n) + ").png");
  pixel3d::write_file_atomic(out, pixel3d::encode_png(*img));
  return out;
}

// Turnaround sheet: figures side by side with clear gaps (same rule as the app's "Split a Turnaround Sheet").
std::vector<fs::path> split_sheet(const fs::path& png, const fs::path& out_dir) {
  auto bytes = pixel3d::read_file(png, 256u << 20);
  if (!bytes) return {};
  auto dec = pixel3d::decode_image(*bytes, u8(png.filename()));
  if (!dec) return {};
  const auto& img = *dec;
  const int W = img.width(), H = img.height();
  bool alpha = false;
  for (int y = 0; y < H && !alpha; y += 4)
    for (int x = 0; x < W; x += 4)
      if (img.at(x, y).a < 128) { alpha = true; break; }
  std::vector<int> r, g, b;
  for (int x = 0; x < W; ++x)
    for (int y : {0, H - 1}) { r.push_back(img.at(x, y).r); g.push_back(img.at(x, y).g); b.push_back(img.at(x, y).b); }
  auto median = [](std::vector<int>& v) { std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end()); return v[v.size() / 2]; };
  const int br = median(r), bg = median(g), bb = median(b);
  auto fg = [&](int x, int y) {
    const auto& p = img.at(x, y);
    if (alpha) return p.a >= 128;
    return std::abs(p.r - br) + std::abs(p.g - bg) + std::abs(p.b - bb) > 60;
  };
  std::vector<int> column(size_t(W), 0);
  for (int x = 0; x < W; ++x)
    for (int y = 0; y < H; ++y) column[size_t(x)] += fg(x, y);
  const int empty = std::max(1, H / 500);
  std::vector<std::pair<int, int>> runs, merged;
  for (int x = 0; x < W;) {
    while (x < W && column[size_t(x)] <= empty) ++x;
    const int s = x;
    while (x < W && column[size_t(x)] > empty) ++x;
    if (x > s) runs.push_back({s, x});
  }
  for (const auto& run : runs) {
    if (!merged.empty() && run.first - merged.back().second < std::max(2, W / 60)) merged.back().second = run.second;
    else merged.push_back(run);
  }
  merged.erase(std::remove_if(merged.begin(), merged.end(), [&](const auto& m) { return m.second - m.first < std::max(4, W / 40); }), merged.end());
  // A sheet: 2..6 figures, each clearly taller than wide (a single wide object is not a sheet).
  if (merged.size() < 2 || merged.size() > 6) return {};
  std::vector<fs::path> out;
  for (size_t k = 0; k < merged.size(); ++k) {
    int y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
      for (int x = merged[k].first; x < merged[k].second; ++x)
        if (fg(x, y)) { y0 = std::min(y0, y); y1 = std::max(y1, y); break; }
    if (y1 < y0 || (y1 - y0) < (merged[k].second - merged[k].first)) return {};
    const int m = std::max(4, (merged[k].second - merged[k].first) / 20);
    const int x0 = std::max(0, merged[k].first - m), x1 = std::min(W, merged[k].second + m), ya = std::max(0, y0 - m), yb = std::min(H, y1 + 1 + m);
    pixel3d::ImageRGBA part(x1 - x0, yb - ya);
    for (int y = ya; y < yb; ++y)
      for (int x = x0; x < x1; ++x) part.at(x - x0, y - ya) = img.at(x, y);
    fs::create_directories(out_dir);
    const fs::path f = out_dir / (safe_name(u8(png.stem())) + "-" + std::to_string(k + 1) + ".png");
    pixel3d::write_file_atomic(f, pixel3d::encode_png(part));
    out.push_back(f);
  }
  return out;
}

// A saved TRELLIS generation of exactly this picture (same bytes, SHA-256) in a 3re project: the newest run with its
// geometry and native texture. Generating again would give the same shape (same seed) and needs 3.25 GiB free RAM.
struct Saved {
  fs::path run, native, project;
};
std::optional<Saved> saved_generation(const fs::path& projects, const fs::path& picture) {
  if (projects.empty() || !fs::exists(projects)) return std::nullopt;
  auto bytes = pixel3d::read_file(picture, 256u << 20);
  if (!bytes) return std::nullopt;
  const std::string want = pixel3d::sha256_hex(std::span<const uint8_t>(*bytes));
  std::optional<Saved> best;
  fs::file_time_type best_time{};
  std::error_code ec;
  for (const auto& proj : fs::directory_iterator(projects, ec)) {
    if (!proj.is_directory() || !fs::exists(proj.path() / "source")) continue;
    bool same = false;
    for (const auto& f : fs::directory_iterator(proj.path() / "source", ec)) {
      if (!f.is_regular_file() || f.file_size() != bytes->size()) continue;
      auto other = pixel3d::read_file(f.path(), 256u << 20);
      if (other && pixel3d::sha256_hex(std::span<const uint8_t>(*other)) == want) same = true;
    }
    if (!same || !fs::exists(proj.path() / "runs")) continue;
    for (const auto& run : fs::directory_iterator(proj.path() / "runs", ec)) {
      if (!fs::exists(run.path() / "geometry.glb") || !fs::exists(run.path() / "native.post")) continue;
      const auto t = fs::last_write_time(run.path() / "geometry.glb", ec);
      if (!best || t > best_time) {
        best = Saved{run.path(), run.path() / "native.post", proj.path()};
        best_time = t;
      }
    }
  }
  return best;
}

// The last non-empty line of a worker's error output (its own reason for failing).
std::string last_line(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  std::string line, last;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (!line.empty()) last = line;
  }
  return last;
}

bool start_detached(const fs::path& exe, const fs::path& spec, const fs::path& out, const std::vector<std::string>& extra, std::string& error) {
  // Detached (no console, own process group, out of the app's job when allowed): the run survives closing 3re;
  // the app and the chat reattach to its folder (W5).
  std::wstring cmd = p3dagent::quote_windows_arg(exe.wstring()) + L" " + p3dagent::quote_windows_arg(spec.wstring()) + L" --out " +
                     p3dagent::quote_windows_arg(out.wstring()) + L" --ask --wait-seconds 3600";
  for (const auto& a : extra) cmd += L" " + p3dagent::quote_windows_arg(p3dagent::widen(a));
  const fs::path console = fs::path(out).concat(L".console.txt");
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE log = CreateFileW(console.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  if (log != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nullptr;
  }
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);
  DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;
  BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi);
  if (!ok && GetLastError() == ERROR_ACCESS_DENIED)  // the job forbids breakaway: still detached from the window
    ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, flags & ~DWORD(CREATE_BREAKAWAY_FROM_JOB), nullptr, nullptr, &si, &pi);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (!ok) {
    error = "3re-agent did not start (Windows error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace

int count_agent_processes() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof e;
  int n = 0;
  for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
    if (_wcsicmp(e.szExeFile, L"3re-agent.exe") == 0) ++n;
  CloseHandle(snap);
  return n;
}

uint64_t free_ram_mib() { return p3dagent::probe_system(fs::temp_directory_path()).ram_free_mib; }

namespace {

// What an agent run painted with and could take a region from (from its report.json and work folder).
struct RunTextures {
  fs::path glb, source;
  std::vector<fs::path> candidates;
};
std::optional<RunTextures> run_textures(const fs::path& run, std::string& error) {
  const json r = read_json(run / "report.json");
  if (!r.is_object()) {
    error = "the run has no report yet";
    return std::nullopt;
  }
  RunTextures t;
  t.glb = from_u8(r.value("texture", json::object()).value("glb", std::string()));
  if (t.glb.empty() || !fs::exists(t.glb)) {
    error = "the run's textured model was not found";
    return std::nullopt;
  }
  const json in = r.value("spec", json::object()).value("input", json::object());
  const json cut = r.value("source", json::object()).value("cutout", json());
  if (cut.is_string() && fs::exists(from_u8(cut.get<std::string>()))) t.source = from_u8(cut.get<std::string>());  // what the worker painted from
  else if (in.value("picture", json()).is_string()) t.source = from_u8(in["picture"].get<std::string>());
  else if (in.value("reuse_run", json()).is_string()) {
    const fs::path rr = from_u8(in["reuse_run"].get<std::string>());
    t.source = fs::exists(rr / "texture-source.png") ? rr / "texture-source.png" : rr / "input.png";
  }
  for (const auto& e : fs::directory_iterator(t.glb.parent_path())) {
    const std::string n = u8(e.path().filename());
    if (e.path() != t.glb && e.path().extension() == ".glb" &&
        (n.rfind("textured-candidate-", 0) == 0 || n.rfind("textured-alternative-", 0) == 0 || n.rfind("textured-decimated-", 0) == 0 ||
         n.rfind("textured-source-", 0) == 0))
      t.candidates.push_back(e.path());
  }
  std::sort(t.candidates.begin(), t.candidates.end());
  return t;
}

}  // namespace

Admission admit_run(bool generates, uint64_t free_mib, int agents_running) {
  // Measured needs (M3 report): the TRELLIS wizard refuses below 3.25 GiB free; texture replays peak at ~3.7 GB, which
  // the agent's own steps manage with their job memory limits once it runs. One agent run at a time: two runs would put
  // generation or texture replays side by side on a 16 GB laptop.
  Admission a;
  a.needed_mib = generates ? 3328 : 2048;
  if (agents_running > 0) {
    a.reason = "another agent run is working (" + std::to_string(agents_running) + " 3re-agent process): heavy steps run one at a time";
    return a;
  }
  if (free_mib < a.needed_mib) {
    a.reason = std::string(generates ? "generation" : "the texture steps") + " need about " + std::to_string(a.needed_mib) + " MiB free RAM, " +
               std::to_string(free_mib) + " MiB are free now: close other programs (browsers, chat apps) or wait";
    return a;
  }
  a.start = true;
  a.reason = std::to_string(free_mib) + " MiB free RAM (needs about " + std::to_string(a.needed_mib) + "), no other agent run";
  return a;
}

json tool_schemas() {
  auto obj = [](json props, std::vector<std::string> req) {
    return json{{"type", "object"}, {"properties", props}, {"required", req}, {"additionalProperties", false}};
  };
  const json str = {{"type", "string"}};
  return json::array({
      {{"name", "propose_labels"},
       {"description", "Split turnaround sheets and propose which side each picture shows (front, back, left, right, three-quarter) "
                       "from file names; 'auto' means the texture worker's registration finds the angle. Labels are proposals: the "
                       "registration on the generated mesh verifies or rejects each one."},
       {"input_schema", obj({{"pictures", {{"type", "array"}, {"items", str}}}, {"labels", {{"type", "object"}}}}, {"pictures"})}},
      {{"name", "make_spec"},
       {"description", "Write the asset spec for the agent from what the person asked for. Required: asset_type, metres, a picture or "
                       "glb, and a platform or triangle_budget."},
       {"input_schema", obj({{"name", str}, {"asset_type", {{"type", "string"}, {"enum", {"character", "prop"}}}},
                             {"platform", {{"type", "string"}, {"enum", {"mobile", "pc", "hero"}}}}, {"triangle_budget", {{"type", "integer"}}},
                             {"texture_size", {{"type", "integer"}}}, {"metres", {{"type", "number"}}},
                             {"axis", {{"type", "string"}, {"enum", {"height", "longest"}}}}, {"requested_style", str}, {"picture", str},
                             {"glb", str}, {"original_picture", str}, {"reuse_saved", {{"type", "boolean"}}}, {"views", {{"type", "array"}}}},
                            {"asset_type", "metres"})}},
      {{"name", "validate_spec"}, {"description", "Check a spec with the agent's own validator."}, {"input_schema", obj({{"spec", str}}, {"spec"})}},
      {{"name", "run_agent"},
       {"description", "Start the Game-Ready Asset Agent on a validated spec (it runs in the background and asks here when it cannot decide)."},
       {"input_schema", obj({{"spec", str}}, {"spec"})}},
      {{"name", "run_status"}, {"description", "Read the state of an agent run from its folder (running, waiting for an answer, finished with its measured result)."},
       {"input_schema", obj({{"run", str}}, {"run"})}},
      {{"name", "answer_decision"},
       {"description", "Answer the agent's open question with one of its option ids (and a picture + side label for picture options)."},
       {"input_schema", obj({{"run", str}, {"question", str}, {"choice", str}, {"picture", str}, {"label", str}}, {"run", "question", "choice"})}},
      {{"name", "open_report"}, {"description", "The agent's report (report.md) of a finished run, with its measured lines."},
       {"input_schema", obj({{"run", str}}, {"run"})}},
      {{"name", "retexture_with_pictures"},
       {"description", "Paint a finished model again with more pictures (back, sides, three-quarters): same generation, same budget "
                       "and size; each picture gets the side the person named or 'auto'; the registration verifies or rejects each one."},
       {"input_schema", obj({{"run", str}, {"pictures", {{"type", "array"}, {"items", {{"type", "object"}}}}}}, {"run", "pictures"})}},
      {{"name", "region_views"},
       {"description", "Render the finished model from 8 angles (same framing as regional_fix) so the person can mark a problem region."},
       {"input_schema", obj({{"run", str}}, {"run"})}},
      {{"name", "regional_fix"},
       {"description", "Diagnose and try to fix a marked region (circle x, y, r in pixels of the yaw's view): what painted it, whether a "
                       "picture sees it, at most 3 attempts taking only the region from other textures of the same mesh, each re-checked; "
                       "nothing outside the region may change (measured). The person decides."},
       {"input_schema", obj({{"run", str}, {"yaw", {{"type", "number"}}}, {"x", {{"type", "number"}}}, {"y", {{"type", "number"}}},
                             {"r", {{"type", "number"}}}, {"note", str}},
                            {"run", "yaw", "x", "y", "r"})}},
  });
}

Tools::Tools(ToolEnv env) : env_(std::move(env)) {
  if (!env_.free_ram_mib) env_.free_ram_mib = [] { return p3dchat::free_ram_mib(); };
  if (!env_.agents_running) env_.agents_running = [] { return p3dchat::count_agent_processes(); };
  started_by_test_ = bool(env_.start_agent);
  if (!env_.start_agent) {
    const fs::path exe = env_.agent_exe;
    const auto extra = env_.agent_args;
    env_.start_agent = [exe, extra](const fs::path& spec, const fs::path& out, std::string& error) { return start_detached(exe, spec, out, extra, error); };
  }
}

json Tools::call(const ToolCall& c) {
  try {
    if (c.name == "propose_labels") return propose_labels(c.args);
    if (c.name == "make_spec") return make_spec(c.args);
    if (c.name == "validate_spec") return validate_spec(c.args);
    if (c.name == "run_agent") return run_agent(c.args);
    if (c.name == "run_status") return run_status(c.args);
    if (c.name == "answer_decision") return answer_decision(c.args);
    if (c.name == "open_report") return open_report(c.args);
    if (c.name == "region_views") return region_views(c.args);
    if (c.name == "regional_fix") return regional_fix(c.args);
    if (c.name == "retexture_with_pictures") return retexture_with_pictures(c.args);
    return fail("unknown tool " + c.name);
  } catch (const std::exception& e) {
    return fail(std::string("tool ") + c.name + " failed: " + e.what());
  }
}

json Tools::propose_labels(const json& a) {
  const fs::path inputs = env_.conversation_dir / "inputs";
  json views = json::array(), sheets = json::array(), errors = json::array();
  std::vector<std::pair<fs::path, fs::path>> pngs;  // (original, png)
  std::vector<fs::path> sheet_parts;                   // their names repeat the sheet's name: no label from it
  for (const auto& p : a.value("pictures", json::array())) {
    if (!p.is_string()) continue;
    const fs::path src = from_u8(p.get<std::string>());
    std::string error;
    auto png = to_png(src, inputs, error);
    if (!png) {
      errors.push_back(error);
      continue;
    }
    const auto parts = split_sheet(*png, inputs / "sheet-parts");
    if (!parts.empty()) {
      json names = json::array();
      for (const auto& q : parts) names.push_back(u8(q));
      sheets.push_back({{"sheet", u8(src)}, {"parts", names}});
      for (const auto& q : parts) pngs.push_back({q, q});
      sheet_parts.insert(sheet_parts.end(), parts.begin(), parts.end());
    } else {
      pngs.push_back({src, *png});
    }
  }
  const json given = a.value("labels", json::object());
  int fronts = 0;
  std::string front, front_original;
  for (const auto& [orig, png] : pngs) {
    std::string label = given.is_object() ? given.value(u8(orig), std::string()) : std::string();
    std::string source = label.empty() ? "file name" : "brain";
    const bool part = std::find(sheet_parts.begin(), sheet_parts.end(), orig) != sheet_parts.end();
    if (label.empty() && !part) label = label_from_name(orig);
    if (label.empty()) {
      label = "auto";
      source = "none: the registration will find the angle";
    }
    if (label == "front") {
      ++fronts;
      if (front.empty()) front = u8(png), front_original = u8(orig);
      continue;
    }
    views.push_back({{"picture", u8(png)}, {"original", u8(orig)}, {"label", label}, {"source", source}});
  }
  // The front: the picture named front; a single picture is its own front; a sheet's first figure is its front
  // (sheets read front, side, back, side). Otherwise the brain or the person must say which one.
  bool needs_front = false;
  if (front.empty()) {
    if (pngs.size() == 1 || (!sheets.empty() && pngs.size() >= 2 && sheets.size() == 1 && pngs.size() == sheets[0]["parts"].size())) {
      front = u8(pngs.front().second);
      front_original = u8(pngs.front().first);
      views.erase(views.begin());
    } else {
      needs_front = true;
    }
  }
  if (views.size() > 5) {
    errors.push_back("at most 5 extra pictures are used; the rest were left out");
    views.erase(views.begin() + 5, views.end());
  }
  return {{"ok", errors.empty() || !pngs.empty()}, {"front", front}, {"front_original", front_original}, {"needs_front", needs_front}, {"fronts_named", fronts}, {"views", views},
          {"sheets", sheets}, {"errors", errors},
          {"note", "labels are proposals; the texture worker's registration verifies each one on the generated mesh"}};
}

json Tools::make_spec(const json& a) {
  const std::string type = a.value("asset_type", std::string());
  if (type != "character" && type != "prop") return fail("asset_type must be character or prop");
  if (!a.contains("metres") || !a["metres"].is_number()) return fail("metres (the real size) is required");
  json input = json::object();
  const std::string picture = a.value("picture", std::string()), glb = a.value("glb", std::string());
  if (!glb.empty()) input["glb"] = glb;
  if (!picture.empty()) {
    // The front picture goes in as PNG inside the conversation (the person's file stays untouched).
    const fs::path p = from_u8(picture);
    if (pixel3d::to_utf8(p.extension()) == ".png" && p.parent_path() == env_.conversation_dir / "inputs") input["picture"] = picture;
    else {
      std::string error;
      auto png = to_png(p, env_.conversation_dir / "inputs", error);
      if (!png) return fail(error);
      input["picture"] = u8(*png);
    }
  }
  if (input.empty()) return fail("a picture or a GLB is required");
  json reused = nullptr;
  if (!picture.empty() && glb.empty() && a.value("reuse_saved", true))
    if (auto saved = saved_generation(env_.projects_dir, from_u8(a.value("original_picture", picture)))) {
      input["reuse_run"] = u8(saved->run);
      input["native_texture"] = u8(saved->native);
      reused = {{"run", u8(saved->run)}, {"project", u8(saved->project)}, {"why", "same picture (SHA-256) already generated in this project"}};
    }
  json views = json::array();
  for (const auto& v : a.value("views", json::array()))
    if (v.is_object() && v.contains("picture")) views.push_back({{"picture", v["picture"]}, {"label", v.value("label", std::string("auto"))}});
  if (!views.empty()) input["views"] = views;
  input["seed"] = 42;

  const std::string platform = a.value("platform", std::string());
  json spec = {{"schema", "3re.asset-spec/1"}, {"input", input}, {"target", {{"engine", "godot"}}}, {"asset_type", type}};
  if (!platform.empty()) {
    const std::string preset = platform == "hero" ? (type == "character" ? "hero_character" : "pc_prop") : platform + "_" + type;
    spec["preset"] = preset;
  }
  if (a.contains("triangle_budget")) spec["triangle_budget"] = a["triangle_budget"];
  if (platform.empty() && !a.contains("triangle_budget")) return fail("a platform (mobile, pc, hero) or a triangle budget is required");
  if (a.contains("texture_size")) spec["texture_size"] = a["texture_size"];
  spec["scale"] = {{"axis", a.value("axis", type == "character" ? std::string("height") : std::string("longest"))}, {"metres", a["metres"]}};
  if (a.contains("requested_style") && !a.value("requested_style", std::string()).empty()) spec["requested_style"] = a["requested_style"];
  spec["limits"] = {{"total_seconds", 3600}, {"step_seconds", 1800}, {"vram_mib", 3584}};
  std::string name = a.value("name", std::string());
  if (name.empty()) name = !picture.empty() ? u8(from_u8(picture).stem()) : !glb.empty() ? u8(from_u8(glb).stem()) : type;
  name = safe_name(name);
  spec["name"] = name;
  const std::string tag = name + "-" + stamp();
  spec["output_dir"] = u8(env_.runs_dir / tag);
  const fs::path file = env_.conversation_dir / "specs" / (tag + ".json");
  fs::create_directories(file.parent_path());
  if (pixel3d::write_file_atomic(file, spec.dump(1)).code != "ok") return fail("cannot write " + u8(file));
  return {{"ok", true}, {"spec", u8(file)}, {"content", spec}, {"reused_generation", reused}};
}

json Tools::validate_spec(const json& a) {
  const auto r = p3dagent::load_spec(from_u8(a.value("spec", std::string())));
  if (!r.spec) return {{"ok", false}, {"errors", r.errors}};
  return {{"ok", true}, {"spec", a["spec"]}, {"triangle_budget", r.spec->triangle_budget}, {"texture_size", r.spec->texture_size},
          {"preset", r.spec->preset}, {"output_dir", u8(r.spec->output_dir)}};
}

json Tools::run_agent(const json& a) {
  const fs::path spec = from_u8(a.value("spec", std::string()));
  const auto r = p3dagent::load_spec(spec);
  if (!r.spec) return {{"ok", false}, {"errors", r.errors}};
  const fs::path out = r.spec->output_dir;
  if (fs::exists(out) && !fs::is_empty(out)) return fail("the run folder exists and is not empty: " + u8(out));
  if (!started_by_test_ && !fs::exists(env_.agent_exe)) return fail("3re-agent.exe was not found at " + u8(env_.agent_exe));
  // W5: measured before the start; when the machine is not ready the run waits (nothing is started half-way).
  const bool generates = r.spec->kind == p3dagent::InputKind::Picture;
  auto adm = admit_run(generates, env_.free_ram_mib(), env_.agents_running());
  // A run that has just written its report takes a moment to exit: re-measure for up to 15 s before refusing.
  for (int i = 0; i < 15 && !adm.start && env_.agents_running() > 0; ++i) {
    Sleep(1000);
    adm = admit_run(generates, env_.free_ram_mib(), env_.agents_running());
  }
  if (!adm.start) return {{"ok", false}, {"waiting", true}, {"reason", adm.reason}, {"needed_mib", adm.needed_mib}, {"spec", u8(spec)}};
  fs::create_directories(out.parent_path());
  std::string error;
  if (!env_.start_agent(spec, out, error)) return fail(error);
  return {{"ok", true}, {"run", u8(out)}, {"spec", u8(spec)}, {"mode", "ask (questions come to this chat)"}, {"admitted", adm.reason}};
}

namespace {
// JSON without null members (a report's spec echo carries nulls the spec parser does not accept).
json without_nulls(const json& j) {
  if (j.is_object()) {
    json o = json::object();
    for (const auto& [k, v] : j.items())
      if (!v.is_null()) o[k] = without_nulls(v);
    return o;
  }
  if (j.is_array()) {
    json a = json::array();
    for (const auto& v : j) a.push_back(without_nulls(v));
    return a;
  }
  return j;
}
}  // namespace

json Tools::retexture_with_pictures(const json& a) {
  const fs::path run = from_u8(a.value("run", std::string()));
  const json report = read_json(run / "report.json");
  if (!report.is_object()) return fail("the run has no report yet: wait until it has finished");
  // The spec the run was started with: the conversation's own spec file, else the report's echo of it.
  json spec;
  if (fs::exists(env_.conversation_dir / "specs"))
    for (const auto& e : fs::directory_iterator(env_.conversation_dir / "specs")) {
      const json s = read_json(e.path());
      if (s.is_object() && fs::path(from_u8(s.value("output_dir", std::string()))).lexically_normal() == run.lexically_normal()) spec = s;
    }
  if (spec.is_null()) {
    spec = without_nulls(report.value("spec", json::object()));
    if (spec.contains("input")) spec["input"].erase("kind");
    spec["schema"] = "3re.asset-spec/1";
  }
  // The same generation: what the run actually used (no second TRELLIS run for a re-texture).
  const json source = report.value("source", json::object());
  const fs::path gen = from_u8(source.value("run", std::string()));
  if (!gen.empty() && fs::exists(gen / "geometry.glb")) {
    spec["input"]["reuse_run"] = u8(gen);
    if (fs::exists(gen / "native.post")) spec["input"]["native_texture"] = u8(gen / "native.post");
    spec["input"].erase("glb");
  }
  json views = spec["input"].value("views", json::array());
  json added = json::array();
  for (const auto& p : a.value("pictures", json::array())) {
    // A picture already in the spec whose side the person corrected: {"replace": N (1-based, as view-N-… in the
    // report), "label": side} — its label is replaced, nothing is added.
    if (p.is_object() && p.contains("replace") && p["replace"].is_number_integer()) {
      const int i = p["replace"].get<int>() - 1;
      if (i < 0 || i >= int(views.size())) return fail("no extra picture number " + std::to_string(i + 1) + " in the run's spec");
      const std::string old = views[size_t(i)].value("label", std::string());
      views[size_t(i)]["label"] = p.value("label", std::string("auto"));
      added.push_back({{"picture", views[size_t(i)]["picture"]}, {"label", views[size_t(i)]["label"]}, {"was", old},
                       {"original", views[size_t(i)]["picture"]}});
      continue;
    }
    if (!p.is_object() || !p.contains("path")) continue;
    std::string error;
    auto png = to_png(from_u8(p["path"].get<std::string>()), env_.conversation_dir / "inputs", error);
    if (!png) return fail(error);
    const std::string label = p.value("label", std::string("auto"));
    views.push_back({{"picture", u8(*png)}, {"label", label}});
    added.push_back({{"picture", u8(*png)}, {"label", label}, {"original", p["path"]}});
  }
  if (added.empty()) return fail("no picture to add or correct");
  if (views.size() > 5) return fail("at most 5 extra pictures can paint a model (this would be " + std::to_string(views.size()) + ")");
  spec["input"]["views"] = views;
  const std::string name = spec.value("name", std::string("asset"));
  const std::string tag = name + "-pictures-" + stamp();
  spec["output_dir"] = u8(env_.runs_dir / tag);
  const fs::path file = env_.conversation_dir / "specs" / (tag + ".json");
  fs::create_directories(file.parent_path());
  if (pixel3d::write_file_atomic(file, spec.dump(1)).code != "ok") return fail("cannot write " + u8(file));
  const auto v = p3dagent::load_spec(file);
  if (!v.spec) return {{"ok", false}, {"errors", v.errors}, {"spec", u8(file)}};
  json r = run_agent({{"spec", u8(file)}});
  r["added_pictures"] = added;
  r["from_run"] = u8(run);
  return r;
}

json read_run_state(const fs::path& run) {
  json s = {{"run", u8(run)}};
  if (!fs::exists(run)) {
    s["state"] = "not started";
    return s;
  }
  // run.jsonl: the tool running now and the last event (complete lines only).
  std::ifstream in(run / "run.jsonl", std::ios::binary);
  std::string line, running, last_event;
  int lines = 0;
  while (std::getline(in, line)) {
    if (line.empty() || line.back() != '}') continue;
    json j;
    try {
      j = json::parse(line);
    } catch (...) {
      continue;
    }
    ++lines;
    if (j.value("event", std::string()) == "step_started") running = j.value("tool", std::string());
    else if (j.contains("step") && j.contains("exit_code")) running.clear();
    if (j.contains("event")) last_event = j["event"].get<std::string>();
  }
  s["log_lines"] = lines;
  const json report = read_json(run / "report.json");
  if (report.is_object()) {
    s["state"] = "finished";
    s["status"] = report.value("status", std::string());
    s["summary"] = report.value("summary", std::string());
    json acc = json::array();
    for (const auto& c : report.value("acceptance", json::array()))
      acc.push_back({{"criterion", c.value("criterion", "")}, {"status", c.value("status", "")}, {"detail", c.value("detail", "")}});
    s["acceptance"] = acc;
    s["deliverables"] = report.value("deliverables", json::array());
    if (report.contains("pictures")) s["pictures"] = report["pictures"];
    if (report.contains("requested_style")) s["requested_style"] = report["requested_style"];
    s["pending_decisions"] = report.value("pending_decisions", json::array()).size();
    s["report"] = u8(run / "report.md");
    return s;
  }
  // An open question: a question file without an answer file.
  const fs::path dec = run / "decisions";
  if (fs::exists(dec)) {
    std::vector<fs::path> qs;
    for (const auto& e : fs::directory_iterator(dec)) {
      const auto n = u8(e.path().filename());
      if (n.size() > 14 && n.substr(n.size() - 14) == ".question.json") qs.push_back(e.path());
    }
    std::sort(qs.begin(), qs.end());
    for (const auto& q : qs) {
      const std::string id = u8(q.filename()).substr(0, u8(q.filename()).size() - 14);
      if (fs::exists(dec / (id + ".answer.json"))) continue;
      const json qj = read_json(q);
      if (!qj.is_object()) continue;
      json opts = json::array();
      for (const auto& o : qj.value("options", json::array()))
        opts.push_back({{"id", o.value("id", "")}, {"label", o.value("label", "")}, {"needs_picture", o.value("needs_picture", false)}});
      s["state"] = "waiting";
      s["question"] = {{"id", id}, {"kind", qj.value("kind", "")}, {"title", qj.value("title", "")}, {"why", qj.value("why", "")},
                       {"options", opts}, {"numbers", qj.value("numbers", json::object())}};
      return s;
    }
  }
  s["state"] = "running";
  s["running_tool"] = running;
  s["last_event"] = last_event;
  return s;
}

json Tools::run_status(const json& a) {
  json s = read_run_state(from_u8(a.value("run", std::string())));
  s["ok"] = true;
  return s;
}

json Tools::answer_decision(const json& a) {
  const fs::path run = from_u8(a.value("run", std::string()));
  p3dagent::Answer ans;
  ans.id = a.value("question", std::string());
  ans.choice = a.value("choice", std::string());
  ans.who = env_.who.empty() ? std::string("unknown user") : env_.who;
  ans.when = p3dagent::now_iso();
  ans.source = "chat";
  if (a.contains("picture") && a["picture"].is_string() && !a["picture"].get<std::string>().empty()) {
    std::string error;
    auto png = to_png(from_u8(a["picture"].get<std::string>()), env_.conversation_dir / "inputs", error);
    if (!png) return fail(error);
    ans.picture = u8(fs::absolute(*png));
    ans.label = a.value("label", std::string("back"));
  }
  const json state = read_run_state(run);
  if (state.contains("question"))
    for (const auto& o : state["question"].value("options", json::array()))
      if (o.value("id", std::string()) == ans.choice && o.value("needs_picture", false) && ans.picture.empty())
        return fail("this option needs a picture: attach the picture of that side");
  std::string error;
  if (!p3dagent::write_answer_file(run, ans, error)) return fail("answer refused: " + error);
  return {{"ok", true}, {"question", ans.id}, {"choice", ans.choice}, {"who", ans.who}, {"when", ans.when}, {"picture", ans.picture}, {"label", ans.label}};
}

json Tools::open_report(const json& a) {
  const fs::path run = from_u8(a.value("run", std::string()));
  json s = read_run_state(run);
  if (s.value("state", std::string()) != "finished") return fail("the run has not finished: no report yet");
  s["ok"] = true;
  return s;
}

json Tools::region_views(const json& a) {
  const fs::path run = from_u8(a.value("run", std::string()));
  std::string error;
  const auto t = run_textures(run, error);
  if (!t) return fail(error);
  const fs::path out = env_.conversation_dir / "regions" / ("views-" + stamp());
  p3dagent::ProcessSpec p;
  p.exe = env_.agent_exe.parent_path() / "pixel3d-regional-fix.exe";
  p.args = {"views", u8(t->glb), u8(out)};
  p.timeout_seconds = 300;
  fs::create_directories(out.parent_path());
  p.stderr_file = fs::path(out).concat(L".err.txt");
  const auto res = p3dagent::run_process(p);
  if (res.exit_code != 0)
    return fail("the region renderer failed" + std::string(res.timed_out ? " (time limit)" : "") + ": " + last_line(p.stderr_file) + res.error);
  json v = read_json(out / "views.json");
  return {{"ok", true}, {"run", u8(run)}, {"views", v.is_object() ? v.value("views", json::array()) : json::array()}, {"dir", u8(out)}};
}

json Tools::regional_fix(const json& a) {
  const fs::path run = from_u8(a.value("run", std::string()));
  std::string error;
  const auto t = run_textures(run, error);
  if (!t) return fail(error);
  const std::string tag = stamp();
  const fs::path out = env_.conversation_dir / "regions" / ("fix-" + tag);
  p3dagent::ProcessSpec p;
  p.exe = env_.agent_exe.parent_path() / "pixel3d-regional-fix.exe";
  auto num = [&](const char* k) { std::ostringstream o; o << a.value(k, 0.0); return o.str(); };
  p.args = {"fix", u8(t->glb), u8(out), "--yaw", num("yaw"), "--mark", num("x"), num("y"), num("r")};
  if (!t->source.empty()) p.args.insert(p.args.end(), {"--source", u8(t->source)});
  for (const auto& c : t->candidates) p.args.insert(p.args.end(), {"--candidate", u8(c)});
  if (!a.value("note", std::string()).empty()) p.args.insert(p.args.end(), {"--note", a["note"].get<std::string>()});
  p.timeout_seconds = 900;
  fs::create_directories(out.parent_path());
  p.stderr_file = out.parent_path() / ("fix-" + tag + ".err.txt");
  const auto res = p3dagent::run_process(p);
  if (res.exit_code != 0) {
    std::string why = last_line(p.stderr_file);
    if (why.rfind("REGIONAL_FIX_ERROR ", 0) == 0) why = why.substr(19);
    return fail("the regional fix failed" + std::string(res.timed_out ? " (time limit)" : "") + ": " + (why.empty() ? "exit " + std::to_string(res.exit_code) : why));
  }
  json f = read_json(out / "fix.json");
  if (!f.is_object()) return fail("the regional fix wrote no report");
  f["ok"] = true;
  f["dir"] = u8(out);
  f["run"] = u8(run);
  f["seconds"] = res.seconds;
  return f;
}

}  // namespace p3dchat
