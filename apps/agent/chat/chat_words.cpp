// The rules brain's reading of words (Arabic and English): asset type, platform, budget, size, texture size,
// style, and what the person wants done. Plain substring and number rules, no model: what it cannot read it asks.
#include <algorithm>
#include <cctype>
#include <regex>

#include "chat.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dchat {

namespace {

std::string lower_ascii(std::string s) {
  for (auto& c : s)
    if (static_cast<unsigned char>(c) < 128) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A word as a whole token (ASCII letters may not touch it); Arabic words match as substrings (prefixes such as
// "ال" / "لل" / "و" attach to them).
bool has_word(const std::string& text, const std::string& w) {
  const bool ascii = std::all_of(w.begin(), w.end(), [](char c) { return static_cast<unsigned char>(c) < 128; });
  size_t pos = 0;
  while ((pos = text.find(w, pos)) != std::string::npos) {
    if (!ascii) return true;
    const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
    const size_t end = pos + w.size();
    const bool right = end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
    if (left && right) return true;
    ++pos;
  }
  return false;
}
bool any_word(const std::string& text, std::initializer_list<const char*> words) {
  for (const char* w : words)
    if (has_word(text, w)) return true;
  return false;
}
double number(const std::string& digits, bool k) {
  std::string s;
  for (char c : digits)
    if (c != ',') s += c;
  try {
    return std::stod(s) * (k ? 1000.0 : 1.0);
  } catch (...) {
    return -1;
  }
}

}  // namespace

bool has_arabic(const std::string& s) {
  for (size_t i = 0; i + 1 < s.size(); ++i) {
    const auto a = static_cast<unsigned char>(s[i]);
    if (a == 0xD8 || a == 0xD9 || a == 0xDA || a == 0xDB) return true;  // U+0600..U+06FF
  }
  return false;
}

std::string ascii_digits(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const auto a = static_cast<unsigned char>(s[i]);
    const auto b = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0;
    if (a == 0xD9 && b >= 0xA0 && b <= 0xA9) {  // U+0660..U+0669 Arabic-Indic
      out += char('0' + (b - 0xA0));
      ++i;
    } else if (a == 0xDB && b >= 0xB0 && b <= 0xB9) {  // U+06F0..U+06F9 Eastern Arabic-Indic
      out += char('0' + (b - 0xB0));
      ++i;
    } else if (a == 0xD9 && b == 0xAB) {  // U+066B Arabic decimal separator
      out += '.';
      ++i;
    } else if (a == 0xD9 && b == 0xAC) {  // U+066C Arabic thousands separator
      out += ',';
      ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string attachment_kind(const fs::path& p) {
  auto ext = lower_ascii(pixel3d::to_utf8(p.extension()));
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp" || ext == ".aseprite" || ext == ".ase" || ext == ".psd")
    return "picture";
  if (ext == ".glb") return "glb";
  return "unsupported";
}

void read_words(const std::string& raw, Draft& d) {
  const std::string t = lower_ascii(ascii_digits(raw));

  if (any_word(t, {"character", "person", "man", "woman", "girl", "boy", "warrior", "knight", "wizard", "mage", "rogue", "monster",
                   "creature", "npc", "enemy", "soldier", "villain", "vampire", "شخصية", "شخصيه", "محارب", "فارس", "ساحر",
                   "وحش", "رجل", "امرأة", "إمرأة", "بنت", "ولد", "مصاص", "جندي", "عدو"}))
    d.asset_type = "character";
  else if (any_word(t, {"prop", "sword", "weapon", "gun", "pistol", "rifle", "axe", "shield", "chair", "table", "bike", "motorcycle",
                        "car", "vehicle", "barrel", "crate", "box", "tool", "lamp", "item", "object", "سيف", "سلاح", "مسدس",
                        "بندقية", "فأس", "درع", "كرسي", "طاولة", "دراجة", "سيارة", "مركبة", "برميل", "صندوق", "أداة", "عنصر",
                        "مجسم", "قطعة"}))
    d.asset_type = "prop";

  if (any_word(t, {"high detail", "cinematic", "hero asset", "عالي التفاصيل", "عالية التفاصيل", "سينمائي"})) d.platform = "hero";
  else if (any_word(t, {"mobile", "phone", "android", "ios", "موبايل", "جوال", "هاتف", "اندرويد", "أندرويد"})) d.platform = "mobile";
  else if (any_word(t, {"pc", "desktop", "console", "steam", "كمبيوتر", "حاسوب", "بي سي", "كونسول", "بلايستيشن"})) d.platform = "pc";

  std::smatch m;
  static const std::regex tris(R"((\d[\d,]*(?:\.\d+)?)\s*(k)?\s*(tris|triangles|triangle|polys|poly|polygons|faces|مثلث|مثلثات|بوليغون|بولي))");
  if (std::regex_search(t, m, tris)) {
    const double v = number(m[1].str(), m[2].matched);
    if (v >= 1) d.triangle_budget = int(v + 0.5);
  }
  static const std::regex cm(R"((\d+(?:\.\d+)?)\s*(cm|سم|سنتيمتر|سنتي))");
  static const std::regex metres(R"((\d+(?:\.\d+)?)\s*(metres|meters|metre|meter|m|متر|أمتار|م)(?![a-z]))");
  // The triangle phrase is removed first: "5000 مثلث" would otherwise read as 5000 م (metres).
  const std::string sizes = std::regex_replace(t, tris, " ");
  if (std::regex_search(sizes, m, cm)) d.metres = number(m[1].str(), false) / 100.0;
  else if (std::regex_search(sizes, m, metres)) {
    // An Arabic letter right after "م" makes it a word (مثلث, مرة), not the unit.
    const size_t end = size_t(m.position(0) + m.length(0));
    const bool arabic_word = m[2].str() == "م" && end + 1 < sizes.size() && (static_cast<unsigned char>(sizes[end]) & 0xF0) == 0xD0;
    if (!arabic_word) d.metres = number(m[1].str(), false);
  }
  if (any_word(t, {"tall", "height", "طول", "طوله", "طولها", "ارتفاع"})) d.axis = "height";
  if (any_word(t, {"long", "length", "longest", "الأطول", "بطول"})) d.axis = d.axis ? *d.axis : "longest";

  if (any_word(t, {"texture", "tex", "نسيج", "تكستشر", "خامة"})) {
    if (any_word(t, {"4096", "4k"})) d.texture_size = 4096;
    else if (any_word(t, {"2048", "2k"})) d.texture_size = 2048;
    else if (any_word(t, {"1024", "1k"})) d.texture_size = 1024;
  }

  static const char* kStyles[] = {"low-poly", "low poly", "lowpoly", "cel-shaded", "cel shaded", "toon", "cartoon", "dark fantasy", "realistic",
                                  "stylized", "stylised", "anime", "pixel art", "hand-painted", "hand painted", "لو بولي", "كرتوني", "كرتون",
                                  "واقعي", "فانتازيا مظلمة", "فانتازيا", "أنمي", "انمي", "مرسوم باليد"};
  std::vector<std::string> styles;
  for (const char* s : kStyles)
    if (has_word(t, s) && std::none_of(styles.begin(), styles.end(), [&](const std::string& x) { return x.find(s) != std::string::npos; }))
      styles.push_back(s);
  if (!styles.empty()) {
    std::string joined;
    for (const auto& s : styles) joined += (joined.empty() ? "" : ", ") + s;
    d.style = joined;
  }

  static const std::regex mark(R"(mark\s+yaw\s*=\s*(-?\d+(?:\.\d+)?)\s+x\s*=\s*(\d+(?:\.\d+)?)\s+y\s*=\s*(\d+(?:\.\d+)?)\s+r\s*=\s*(\d+(?:\.\d+)?))");
  if (std::regex_search(t, m, mark))
    d.mark = json{{"yaw", std::stod(m[1].str())}, {"x", std::stod(m[2].str())}, {"y", std::stod(m[3].str())}, {"r", std::stod(m[4].str())}};
  if (any_word(t, {"unity", "unreal", "ue5", "ue4", "يونتي", "يونيتي", "انريل", "أنريل"})) d.engine_words = "unity/unreal";
  d.wants_rigging = any_word(t, {"rig", "rigging", "rigged", "skeleton", "bones", "skin weights", "ريغ", "تريغ", "هيكل عظمي", "عظام"});
  d.wants_status = any_word(t, {"status", "progress", "how is it going", "where is it", "حالة", "الحالة", "وين وصل", "التقدم", "وصلت"});
  d.wants_report = any_word(t, {"report", "result", "results", "تقرير", "التقرير", "النتيجة", "النتائج"});
  d.wants_cancel = any_word(t, {"cancel", "stop the run", "إلغاء", "الغاء", "أوقف", "اوقف"});
}

json Draft::to_json() const {
  json j = json::object();
  if (asset_type) j["asset_type"] = *asset_type;
  if (platform) j["platform"] = *platform;
  if (triangle_budget) j["triangle_budget"] = *triangle_budget;
  if (texture_size) j["texture_size"] = *texture_size;
  if (metres) j["metres"] = *metres;
  if (axis) j["axis"] = *axis;
  if (!style.empty()) j["requested_style"] = style;
  json pics = json::array(), glb = json::array();
  for (const auto& a : pictures) pics.push_back(pixel3d::to_utf8(a.path));
  for (const auto& a : glbs) glb.push_back(pixel3d::to_utf8(a.path));
  if (!pics.empty()) j["pictures"] = pics;
  if (!glb.empty()) j["glb"] = glb.front();
  return j;
}

}  // namespace p3dchat
