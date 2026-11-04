#include "fixturegen/hand_maps.hpp"

#include <map>
#include <string>
#include <vector>

namespace p3dfix {

using namespace pixel3d;

namespace {

std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Expected<ImageRGBA> image_from_text_map(std::string_view text) {
  const auto lines = split_lines(text);
  std::map<char, Rgba8> palette;
  size_t i = 0;
  if (lines.empty() || lines[0] != "palette:") return make_error("fixture.no_palette", "missing 'palette:' header");
  for (i = 1; i < lines.size() && lines[i] != "---"; ++i) {
    const std::string& l = lines[i];
    if (l.size() != 9 && l.size() != 11) return make_error("fixture.bad_palette", "bad palette line: " + l);
    if (l[1] != '=' || l[2] != '#') return make_error("fixture.bad_palette", "bad palette line: " + l);
    int v[4] = {0, 0, 0, 255};
    for (size_t k = 0; k < (l.size() - 3) / 2; ++k) {
      const int hi = hex(l[3 + 2 * k]), lo = hex(l[4 + 2 * k]);
      if (hi < 0 || lo < 0) return make_error("fixture.bad_palette", "bad hex: " + l);
      v[k] = hi * 16 + lo;
    }
    palette[l[0]] = Rgba8{uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3])};
  }
  std::vector<std::string> rows;
  for (++i; i < lines.size(); ++i)
    if (!lines[i].empty()) rows.push_back(lines[i]);
  if (rows.empty()) return make_error("fixture.no_rows", "no pixel rows");
  const size_t w = rows[0].size();
  for (const auto& r : rows)
    if (r.size() != w) return make_error("fixture.ragged_rows", "rows have different lengths");
  ImageRGBA img(static_cast<int>(w), static_cast<int>(rows.size()));
  for (size_t y = 0; y < rows.size(); ++y)
    for (size_t x = 0; x < w; ++x) {
      const char c = rows[y][x];
      if (c == '.') continue;
      const auto it = palette.find(c);
      if (it == palette.end()) return make_error("fixture.unknown_char", std::string("unknown pixel char: ") + c);
      img.at(static_cast<int>(x), static_cast<int>(y)) = it->second;
    }
  return img;
}

}  // namespace p3dfix
