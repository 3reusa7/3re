// Layered sources (1.2.0): the visible raster layers of .aseprite / .psd views become
// SUGGESTED part masks named after the layer. The user accepts or rejects them; nothing
// is created until apply=true (one undo step). Masks live in aligned view pixels, so a
// layer pixel is mapped through the view's own downscale, crop, mirror and padding.
#include <algorithm>
#include <cctype>
#include <set>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/image/layered.hpp"
#include "pixel3d/image/pixel_scale.hpp"

namespace pixel3d {

using pipeline::Stage;

namespace {

// The source pixel of aligned pixel (u, v), or false when the view was resampled by
// normalisation (no exact mapping) or (u, v) is padding.
bool original_pixel(const SourceView& v, int u, int y, int* ox, int* oy) {
  if (v.working) return false;
  int x = u - v.pad_x, yy = y - v.pad_y;
  if (x < 0 || yy < 0 || x >= v.crop.width() || yy >= v.crop.height()) return false;
  if (v.flipped_x) x = v.crop.width() - 1 - x;
  *ox = block_source(x + v.crop.x0, v.scale.k, v.scale.ox);
  *oy = block_source(yy + v.crop.y0, v.scale.k, v.scale.oy);
  return true;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// A first guess of the Low-Poly Studio type from the layer name (the user can change it).
lowpoly::PartType guess_type(const std::string& name) {
  const std::string n = lower(name);
  auto has = [&](std::initializer_list<const char*> words) {
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return n.find(w) != std::string::npos; });
  };
  if (has({"hair", "horn", "ear", "eye", "hat", "belt", "pouch", "bag", "buckle", "weapon", "sword", "shield"}))
    return lowpoly::PartType::Accessory;
  if (has({"head", "face", "skull", "mask"})) return lowpoly::PartType::Head;
  if (has({"hood", "cloak", "cape", "scarf", "robe", "skirt", "coat", "dress"})) return lowpoly::PartType::Cloth;
  if (has({"arm", "hand", "leg", "foot", "feet", "boot", "shoe", "thigh", "shin", "finger", "tail"}))
    return lowpoly::PartType::Limb;
  if (has({"body", "torso", "chest", "tunic", "shirt", "belly"})) return lowpoly::PartType::Torso;
  return lowpoly::PartType::Accessory;
}

struct Suggested {
  std::string name, path;
  bool visible = true;
  std::map<ViewRole, Mask2D> masks;
};

}  // namespace

OpResult Session::suggest_layer_parts(const LayerPartsRequest& req) {
  auto suggest_into = [&](Document& d, OpResult& r) -> std::optional<Message> {
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Aligned, {}, notes)) return e;
    for (const Message& m : notes) r.add(m);
    std::vector<Suggested> found;  // by layer path, in the order first seen
    bool any_layered = false;
    for (const SourceView& v : d.views) {
      if (!v.original_bytes || !v.aligned) continue;
      const ImageFormat f = sniff_image_format(*v.original_bytes);
      if (f != ImageFormat::Aseprite && f != ImageFormat::Psd) continue;
      any_layered = true;
      auto l = decode_layered(*v.original_bytes);
      if (!l) return l.error();
      for (Message m : l->notes) {
        m.where["view"] = to_string(v.role);
        r.add(std::move(m));
      }
      if (v.working) {
        r.add(Message{"layers.view_resampled",
                      std::string(to_string(v.role)) + ": the drawing was resampled by normalisation, so its layers "
                                                       "cannot be mapped pixel for pixel; no layer masks for this view",
                      Severity::Warning, {{"view", to_string(v.role)}}});
        continue;
      }
      const Mask2D opaque = opaque_mask(*v.aligned, d.settings.alpha_threshold);
      for (const SourceLayer& sl : l->layers) {
        Mask2D m(v.aligned->width(), v.aligned->height());
        for (int y = 0; y < m.height(); ++y)
          for (int u = 0; u < m.width(); ++u) {
            int ox = 0, oy = 0;
            if (!opaque.get(u, y) || !original_pixel(v, u, y, &ox, &oy)) continue;
            const int lx = ox - sl.rect.x0, ly = oy - sl.rect.y0;
            if (lx < 0 || ly < 0 || lx >= sl.pixels.width() || ly >= sl.pixels.height()) continue;
            if (sl.pixels.at(lx, ly).a >= d.settings.alpha_threshold) m.set(u, y, true);
          }
        auto it = std::find_if(found.begin(), found.end(), [&](const Suggested& s) { return s.path == sl.path; });
        if (it == found.end()) {
          found.push_back(Suggested{sl.name, sl.path, sl.visible, {}});
          it = std::prev(found.end());
        }
        it->visible = it->visible && sl.visible;
        if (m.count() > 0) it->masks[v.role] = std::move(m);
      }
    }
    if (!any_layered)
      return Message{"layers.none", "no view comes from a layered file (.aseprite, .ase, .psd)", Severity::Error, {}};
    const bool lowpoly = d.settings.lowpoly.enabled;
    std::set<std::string> wanted(req.accept.begin(), req.accept.end());
    for (const std::string& w : wanted)
      if (std::none_of(found.begin(), found.end(), [&](const Suggested& s) { return s.name == w || s.path == w; }))
        return Message{"input.invalid", "no layer named " + w, Severity::Error, {{"field", "accept"}, {"value", w}}};
    r.data["layers"] = nlohmann::json::array();
    std::vector<int> ids;
    if (req.apply) {
      if (!d.masks.parts.empty() && !req.replace)
        return Message{"layers.parts_exist",
                       std::to_string(d.masks.parts.size()) + " user part(s) exist: apply with replace=true to replace them",
                       Severity::Error, {{"parts", d.masks.parts.size()}}};
      for (const auto& [id, up] : d.masks.parts) {
        d.part_names.erase(id);
        d.part_types.erase(id);
        d.settings.lowpoly.depth_ratio.erase(id);
      }
      d.masks.parts.clear();
      pipeline::clear_from(d, Stage::Matched);
    }
    for (const Suggested& s : found) {
      const bool suggested = s.visible && !s.masks.empty();
      nlohmann::json lj{{"name", s.name}, {"path", s.path}, {"visible", s.visible}, {"suggested", suggested}};
      if (lowpoly) lj["type"] = lowpoly::to_string(guess_type(s.name));
      for (const auto& [role, m] : s.masks) {
        const auto b = m.bbox();
        lj["views"][std::string(to_string(role))] = {{"pixels", m.count()}, {"box", {b.x0, b.y0, b.x1, b.y1}}};
      }
      if (!s.visible) lj["why_not"] = "hidden layer";
      else if (s.masks.empty()) lj["why_not"] = "no drawn pixel of this layer is in the view";
      const bool take = suggested && (wanted.empty() || wanted.contains(s.name) || wanted.contains(s.path));
      if (req.apply && take) {
        const int id = d.next_part_id++;
        UserPart up;
        up.masks = s.masks;
        d.masks.parts[id] = std::move(up);
        d.part_names[id] = s.name;
        if (lowpoly) d.part_types[id] = guess_type(s.name);
        lj["id"] = id;
        ids.push_back(id);
      }
      r.data["layers"].push_back(std::move(lj));
    }
    r.data["applied"] = req.apply;
    if (req.apply) r.data["part_ids"] = ids;
    else
      r.add(Message{"layers.suggestion",
                    "layer masks are suggestions: nothing was changed (apply=true creates the accepted ones)",
                    Severity::Info, {{"layers", found.size()}}});
    return std::nullopt;
  };
  if (!req.apply) {
    try {
      Document d = history_.current();
      OpResult r;
      if (auto e = suggest_into(d, r)) return OpResult::fail(*e);
      r.success = true;
      finish(r);
      return r;
    } catch (const std::exception& e) {
      return OpResult::fail(Message{"internal.exception", e.what(), Severity::Error, {}});
    }
  }
  nlohmann::json params{{"apply", true}, {"replace", req.replace}, {"accept", req.accept}};
  return apply("suggest_layer_parts", params, Commit::Push, [&](Document& d) {
    OpResult r;
    if (auto e = suggest_into(d, r)) return OpResult::fail(*e);
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

}  // namespace pixel3d
