#include "issues.hpp"

#include <QCoreApplication>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "pixel3d/segment/bands.hpp"
#include "pixel3d/validate/metrics.hpp"
#include "qt_util.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;
using theme::Tone;

namespace {

QString tr(const char* s) { return QCoreApplication::translate("Pixel3D", s); }

std::vector<int> int_list(const json& j) {
  std::vector<int> out;
  if (j.is_array())
    for (const auto& x : j)
      if (x.is_number_integer()) out.push_back(x.get<int>());
  return out;
}

QString plural(int n, const char* one, const char* many) { return QString::fromUtf8(n == 1 ? one : many).arg(n); }

Region3 sweep_region(ViewRole view, Dims d, const QRect& box) {
  const double extent = view_depth_extent(view, d);
  const vec3d a = world_from_view(view, d, box.left(), box.top(), 0);
  const vec3d b = world_from_view(view, d, box.left() + box.width(), box.top() + box.height(), extent);
  return {QVector3D(static_cast<float>(std::min(a.x, b.x)), static_cast<float>(std::min(a.y, b.y)),
                    static_cast<float>(std::min(a.z, b.z))),
          QVector3D(static_cast<float>(std::max(a.x, b.x)), static_cast<float>(std::max(a.y, b.y)),
                    static_cast<float>(std::max(a.z, b.z)))};
}

int level_rank(const Issue& i) {
  if (i.level == IssueLevel::Blocking) return i.tone == Tone::Fail ? 0 : 1;
  return i.level == IssueLevel::Warning ? 2 : 3;
}

}  // namespace

QString Issue::key() const {
  QStringList parts{kind};
  for (int c : candidates) parts << QString::number(c);
  if (!box.isNull()) parts << QStringLiteral("%1,%2,%3,%4").arg(box.left()).arg(box.top()).arg(box.width()).arg(box.height());
  if (view) parts << qs(to_string(*view));
  return parts.join('|');
}

QString view_title(ViewRole r) {
  switch (r) {
    case ViewRole::Front: return tr("Front");
    case ViewRole::Back: return tr("Back");
    case ViewRole::Side: return tr("Side");
    case ViewRole::Right: return tr("Right");
    case ViewRole::Top: return tr("Top");
    case ViewRole::Bottom: return tr("Bottom");
  }
  return {};
}

QString view_orientation(ViewRole r) {
  switch (r) {
    case ViewRole::Front: return tr("seen from the front");
    case ViewRole::Back: return tr("seen from behind");
    case ViewRole::Side: return tr("seen from the left");
    case ViewRole::Right: return tr("seen from the right");
    case ViewRole::Top: return tr("from above, front edge down");
    case ViewRole::Bottom: return tr("from below, front edge up");
  }
  return {};
}

QString part_status_words(const QString& s) {
  if (s == "certain") return tr("Seen in the drawings");
  if (s == "certain_palette") return tr("Seen (matched by colour)");
  if (s == "inferred_continuation") return tr("Hidden — continued from neighbours");
  if (s == "inferred_grid") return tr("Hidden — placed on a grid");
  if (s == "inferred_fill") return tr("Hidden — filled in");
  if (s == "ambiguous") return tr("Undecided — views don't agree");
  if (s == "removed") return tr("Removed");
  return s;
}

QString part_name(const Part3D& p) {
  if (!p.label.empty() && p.label.rfind("part_", 0) != 0) return qs(p.label);
  return tr("Part %1").arg(p.id + 1);
}

Tone part_status_tone(const QString& s) {
  if (s.startsWith("inferred")) return Tone::Info;
  if (s == "ambiguous") return Tone::Review;
  if (s == "certain" || s == "certain_palette") return Tone::Pass;
  return Tone::Neutral;
}

QString check_title(const QString& n) {
  const QString view = n.section('_', -1);
  const auto vr = view_role_from_string(utf8(view));
  const QString vname = vr ? view_title(*vr) : view;
  if (n == "mesh_non_empty") return tr("The model is empty");
  if (n == "watertight") return tr("The model's surface has holes");
  if (n == "non_manifold") return tr("The model has broken geometry");
  if (n == "internal_faces") return tr("The model has hidden inner faces");
  if (n == "winding") return tr("Some faces point inward");
  if (n.startsWith("projection_")) return tr("The model doesn't match the %1 drawing").arg(vname);
  if (n.startsWith("color_exact_")) return tr("Colours differ from the %1 drawing").arg(vname);
  if (n.startsWith("rotation_")) return tr("The model looks wrong when turned");
  if (n == "thickness_ratio") return tr("The model is too thin from some side");
  if (n == "turntable_area") return tr("The model nearly disappears at some angles");
  if (n == "glb_roundtrip") return tr("The exported file failed its self-check");
  if (n == "volume_iou_ground_truth") return tr("The model doesn't match the reference");
  return tr("Check failed: %1").arg(n);
}

QString percent(double f) {
  if (f >= 0.9995) return QStringLiteral("100 %");
  return QStringLiteral("%1 %").arg(std::floor(f * 1000.0) / 10.0, 0, 'f', 1);
}

QString project_type_name(bool lowpoly) { return lowpoly ? tr("Low-Poly Studio") : tr("Visual Hull"); }

QString project_type_needs(bool lowpoly) {
  return lowpoly ? tr("Front + Side, or Front only") : tr("needs Front + Side + Top");
}

std::vector<ViewRole> missing_views(const Document& d) {
  std::vector<ViewRole> missing;
  if (d.settings.lowpoly.enabled || d.single_view.enabled) return missing;
  for (ViewRole r : kRequiredViewRoles)
    if (std::none_of(d.views.begin(), d.views.end(), [r](const SourceView& s) { return s.role == r; })) missing.push_back(r);
  return missing;
}

bool lowpoly_can_rescue(const Document& d) {
  const std::vector<ViewRole> m = missing_views(d);
  return !m.empty() && std::find(m.begin(), m.end(), ViewRole::Front) == m.end();
}

QString missing_views_text(const Document& d) {
  const std::vector<ViewRole> m = missing_views(d);
  if (m.size() == 1 && m.front() == ViewRole::Top)
    return tr("Visual Hull needs a Top drawing (from above). No Top? Low-Poly Studio builds from your Front + Side.");
  if (m.size() == 1 && m.front() == ViewRole::Side)
    return tr("Visual Hull needs a Side drawing (seen from the left). Low-Poly Studio builds from what you have.");
  return tr("Visual Hull needs Side and Top drawings. Only a Front? Low-Poly Studio builds from it alone.");
}

QString position_name(QRectF fp, double y0, double y1, Dims d) {
  if (d.W <= 0 || d.D <= 0 || d.H <= 0) return tr("Part");
  const double cx = fp.center().x() / d.W, cz = fp.center().y() / d.D, cy = (y0 + y1) * 0.5 / d.H;
  const QString h = cx < 0.36 ? tr("left") : cx > 0.64 ? tr("right") : tr("centre");
  const QString z = cz > 0.64 ? tr("Front") : cz < 0.36 ? tr("Back") : QString();
  QString name = z.isEmpty() ? (h == tr("centre") ? tr("Centre") : h.left(1).toUpper() + h.mid(1)) : z + " " + h;
  if (cy < 0.36) name += tr(", low");
  else if (cy > 0.64) name += tr(", high");
  return name;
}

QRectF view_rect(const Region3& r, ViewRole view, Dims d) {
  double u0 = 1e30, v0 = 1e30, u1 = -1e30, v1 = -1e30;
  for (int k = 0; k < 8; ++k) {
    const vec3d p{(k & 1) ? r.hi.x() : r.lo.x(), (k & 2) ? r.hi.y() : r.lo.y(), (k & 4) ? r.hi.z() : r.lo.z()};
    const vec3d q = view_from_world(view, d, p);
    u0 = std::min(u0, q.x), u1 = std::max(u1, q.x), v0 = std::min(v0, q.y), v1 = std::max(v1, q.y);
  }
  return QRectF(QPointF(u0, v0), QPointF(u1, v1));
}

Region3 candidate_region(const Document& d, int id) {
  if (!d.candidates || !d.align || id < 0 || id >= static_cast<int>(d.candidates->cands.size())) return {};
  const Candidate& c = d.candidates->cands[static_cast<size_t>(id)];
  const Dims dims = d.align->dims;
  double y0 = 0, y1 = dims.H;
  if (d.bands)
    for (const Band& b : d.bands->bands)
      if (b.id == c.band) y0 = b.y0, y1 = b.y1;
  const vec3d a = world_from_view(ViewRole::Top, dims, c.footprint.x0, c.footprint.y0, 0);
  const vec3d b = world_from_view(ViewRole::Top, dims, c.footprint.x1, c.footprint.y1, 0);
  return {QVector3D(static_cast<float>(std::min(a.x, b.x)), static_cast<float>(y0), static_cast<float>(std::min(a.z, b.z))),
          QVector3D(static_cast<float>(std::max(a.x, b.x)), static_cast<float>(y1), static_cast<float>(std::max(a.z, b.z)))};
}

std::vector<CandidateInfo> candidate_infos(const Document& d, const std::vector<int>& ids) {
  std::vector<CandidateInfo> out;
  if (!d.candidates || !d.align) return out;
  for (int id : ids) {
    if (id < 0 || id >= static_cast<int>(d.candidates->cands.size())) continue;
    const Candidate& c = d.candidates->cands[static_cast<size_t>(id)];
    CandidateInfo ci;
    ci.id = id;
    ci.part = d.correspondence && id < static_cast<int>(d.correspondence->part_of_candidate.size())
                  ? d.correspondence->part_of_candidate[static_cast<size_t>(id)]
                  : -1;
    ci.status = qs(to_string(c.status));
    ci.footprint = QRect(QPoint(c.footprint.x0, c.footprint.y0), QPoint(c.footprint.x1 - 1, c.footprint.y1 - 1));
    ci.region = candidate_region(d, id);
    ci.position = position_name(QRectF(QPointF(c.footprint.x0, c.footprint.y0), QPointF(c.footprint.x1, c.footprint.y1)),
                                ci.region.lo.y(), ci.region.hi.y(), d.align->dims);
    ci.volume = c.volume;
    out.push_back(ci);
  }
  return out;
}

std::vector<Issue> build_issues(const SessionView& v) {
  std::vector<Issue> out;
  const Document& d = v.doc;
  const std::optional<Dims> dims = d.align ? std::optional<Dims>(d.align->dims) : std::nullopt;
  const json list = v.conflicts.is_object() ? v.conflicts.value("conflicts", json::array()) : json::array();
  for (size_t i = 0; i < list.size(); ++i) {
    const json& c = list[i];
    if (!c.is_object()) continue;
    Issue is;
    is.raw = c;
    is.conflict_index = static_cast<int>(i);
    is.kind = qs(c.value("kind", std::string()));
    is.engine_text = qs(c.value("text", std::string()));
    const bool blocking = c.value("blocks_pass", false);
    const std::string sev = c.value("severity", std::string());
    is.level = blocking ? IssueLevel::Blocking : sev == "warning" || sev == "error" ? IssueLevel::Warning : IssueLevel::Note;
    const json where = c.value("where", json::object());
    if (where.is_object()) {
      is.candidates = int_list(where.value("candidates", json::array()));
      for (const auto& alt : where.value("alternatives", json::array())) is.alternatives.push_back(int_list(alt));
      if (where.contains("view") && where["view"].is_string()) is.view = view_role_from_string(where["view"].get<std::string>());
      const json box = where.value("box", json::array());
      if (box.is_array() && box.size() == 4 && box[0].is_number())
        is.box = QRect(QPoint(box[0].get<int>(), box[1].get<int>()), QPoint(box[2].get<int>() - 1, box[3].get<int>() - 1));
    }
    const int n = static_cast<int>(is.candidates.size());
    const QString k = is.kind;
    if (k == "ambiguous_correspondence") {
      is.tone = Tone::Review;
      is.fix = IssueFix::ChooseParts;
      is.title = tr("Parts don't match between views");
      is.bullet = plural(n, "%1 part doesn't match between views", "%1 parts don't match between views");
      is.explanation = is.alternatives.size() > 1
                           ? tr("The drawings fit %1 different arrangements of these parts equally well. Choose which "
                                "parts are real, or add a Back, Right or Bottom view.")
                                 .arg(is.alternatives.size())
                           : tr("The drawings fit more than one arrangement of these parts. Choose which parts are real, "
                                "or add a Back, Right or Bottom view.");
    } else if (k == "evidence_conflict") {
      is.tone = Tone::Review;
      is.fix = IssueFix::ChooseParts;
      is.title = tr("The views disagree about some parts");
      is.bullet = plural(n, "%1 part with conflicting drawings", "%1 parts with conflicting drawings");
      is.explanation = tr("Colours or outlines in one drawing contradict another for these parts. Choose which parts "
                          "are real.");
    } else if (k == "edit.uncovers_atom") {
      is.tone = Tone::Review;
      is.fix = IssueFix::ChooseParts;
      is.title = tr("A removed part leaves the drawing unexplained");
      is.bullet = tr("A removed part leaves a gap");
      is.explanation = tr("After your last decision some drawn pixels are no longer covered by any part. Keep one of "
                          "these parts again.");
    } else if (k == "possible_recess") {
      is.tone = Tone::Review;
      is.fix = IssueFix::SetDepth;
      is.title = tr("Hole or painted detail?");
      is.bullet = tr("1 possible hole — depth unknown");
      is.explanation = tr("A darker enclosed area in the %1 drawing could be a hole, a cavity, or just paint. The "
                          "drawings don't show how deep it goes.")
                           .arg(is.view ? view_title(*is.view) : tr("Top"));
    } else if (k == "extent_mismatch") {
      is.tone = Tone::Fail;
      is.fix = IssueFix::FixDrawings;
      is.title = tr("The drawings don't agree on the model's size");
      is.bullet = tr("Drawings have different sizes");
      is.explanation = tr("Front and Side must have the same height, and Top must match the Front's width and the "
                          "Side's depth. Fix the drawing sizes and import them again.");
    } else if (k == "silhouette_conflict") {
      is.tone = Tone::Neutral;
      is.fix = IssueFix::FixDrawings;
      const int px = where.is_object() ? where.value("pixels", 0) : 0;
      is.title = tr("Some drawn pixels can't be matched");
      is.bullet = tr("%1 unmatched pixels in %2").arg(px).arg(is.view ? view_title(*is.view) : QString());
      is.explanation = tr("%1 pixels of the %2 drawing can't be explained by the other drawings — usually an outline "
                          "that doesn't line up between views.")
                           .arg(px)
                           .arg(is.view ? view_title(*is.view) : tr("a"));
    } else if (k.startsWith("inferred_grid") || k.startsWith("inferred_continuation") || k.startsWith("inferred_fill")) {
      is.tone = Tone::Info;
      is.title = tr("Hidden parts estimated");
      is.bullet = plural(n, "%1 hidden part estimated", "%1 hidden parts estimated");
      is.explanation = k == "inferred_grid" ? tr("No drawing shows these parts directly; they were placed on a grid, "
                                                 "like the legs of a chair.")
                       : k == "inferred_continuation" ? tr("No drawing shows these parts directly; they continue the "
                                                           "parts above and below.")
                                                      : tr("These parts were filled in where every drawing shows "
                                                           "material.");
    } else if (k == "inferred_surface" || k == "inferred_surface_flat") {
      is.tone = Tone::Info;
      is.title = tr("Hidden surface estimated");
      is.bullet = tr("Hidden surfaces estimated");
      is.explanation = k == "inferred_surface_flat"
                           ? tr("More than a tenth of the surface uses a flat palette colour because no drawing shows it.")
                           : tr("Surfaces no drawing shows were painted from nearby art.");
    } else if (k == "user_depth") {
      is.tone = Tone::Neutral;
      is.title = tr("Depth set by you");
      is.explanation = tr("You decided how deep this area goes.");
    } else if (k == "inferred_flush") {
      is.tone = Tone::Neutral;
      is.title = tr("Painted detail kept flat");
      is.explanation = tr("An enclosed painted area is assumed to be flat paint, not a hole.");
    } else if (k == "texture_assignment.skipped") {
      is.tone = Tone::Neutral;
      is.title = tr("Some manual textures were skipped");
      is.explanation = is.engine_text;
    } else {
      is.tone = is.level == IssueLevel::Blocking ? Tone::Review : Tone::Neutral;
      QString t = k;
      t.replace('_', ' ').replace('.', ' ');
      is.title = t.left(1).toUpper() + t.mid(1);
      is.explanation = is.engine_text;
    }
    if (is.bullet.isEmpty()) is.bullet = is.title;
    // Regions and affected parts.
    if (dims) {
      for (int id : is.candidates) {
        const Region3 r = candidate_region(d, id);
        if (r.hi.x() > r.lo.x()) is.regions.push_back(r);
      }
      if (is.view && !is.box.isNull()) is.regions.push_back(sweep_region(*is.view, *dims, is.box));
    }
    std::set<int> parts;
    if (d.correspondence)
      for (int id : is.candidates)
        if (id >= 0 && id < static_cast<int>(d.correspondence->part_of_candidate.size()) &&
            d.correspondence->part_of_candidate[static_cast<size_t>(id)] >= 0)
          parts.insert(d.correspondence->part_of_candidate[static_cast<size_t>(id)]);
    is.parts.assign(parts.begin(), parts.end());
    out.push_back(std::move(is));
  }
  // Failed critical quality checks (only for a current validation).
  if (d.gate && v.status.status != ValidationStatus::NotValidated)
    for (const Check& c : d.gate->checks) {
      if (!c.critical || c.passed) continue;
      Issue is;
      is.level = IssueLevel::Blocking;
      is.tone = Tone::Fail;
      is.fix = IssueFix::FixDrawings;
      is.kind = QStringLiteral("check.%1").arg(qs(c.name));
      is.title = check_title(qs(c.name));
      is.bullet = is.title;
      is.explanation = tr("The model needs fixes before export. Check the drawings for this, reconstruct and validate "
                          "again.");
      is.engine_text = QStringLiteral("%1: value %2, threshold %3. %4")
                           .arg(qs(c.name))
                           .arg(c.value, 0, 'g', 4)
                           .arg(c.threshold, 0, 'g', 4)
                           .arg(qs(c.text));
      is.raw = json{{"name", c.name}, {"value", c.value}, {"threshold", c.threshold}, {"text", c.text}};
      out.push_back(std::move(is));
    }
  std::stable_sort(out.begin(), out.end(), [](const Issue& a, const Issue& b) { return level_rank(a) < level_rank(b); });
  return out;
}

void place_on_surface(std::vector<Issue>& issues, const Scene& s, Dims d) {
  if (s.empty() || d.W <= 0) return;
  for (Issue& is : issues) {
    if (is.fix != IssueFix::SetDepth || !is.view || is.box.isNull() || is.regions.empty()) continue;
    const ViewRole r = *is.view;
    const vec3d vd = view_dir(r);
    const QVector3D dir(static_cast<float>(vd.x), static_cast<float>(vd.y), static_cast<float>(vd.z));
    const QRectF b(is.box.left(), is.box.top(), is.box.width(), is.box.height());
    float best = 1e30f;
    for (const QPointF f : {QPointF(0.5, 0.5), QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.2, 0.8), QPointF(0.8, 0.8)}) {
      const vec3d o = world_from_view(r, d, b.left() + f.x() * b.width(), b.top() + f.y() * b.height(), -1.0);
      float t = 0;
      if (ray_cast(s, QVector3D(static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z)), dir, &t) >= 0)
        best = std::min(best, t - 1.0f);
    }
    if (best > 1e29f) continue;
    const vec3d p = world_from_view(r, d, b.left(), b.top(), best - 0.05);
    const vec3d q = world_from_view(r, d, b.right(), b.bottom(), best + 0.6);
    is.regions.back() = {QVector3D(static_cast<float>(std::min(p.x, q.x)), static_cast<float>(std::min(p.y, q.y)),
                                   static_cast<float>(std::min(p.z, q.z))),
                         QVector3D(static_cast<float>(std::max(p.x, q.x)), static_cast<float>(std::max(p.y, q.y)),
                                   static_cast<float>(std::max(p.z, q.z)))};
  }
}

StatusSummary summarize(const SessionView& v, const std::vector<Issue>& issues) {
  StatusSummary s;
  s.status = v.status.status;
  const bool has_model = v.doc.solid != nullptr;
  for (const Issue& i : issues) s.blocking += i.level == IssueLevel::Blocking;
  switch (s.status) {
    case ValidationStatus::Pass:
      s.word = tr("PASS");
      s.sentence = tr("Ready to export.");
      break;
    case ValidationStatus::NeedsReview:
      s.word = tr("NEEDS REVIEW");
      s.sentence = plural(std::max(1, s.blocking), "%1 issue needs your decision.", "%1 issues need your decision.");
      break;
    case ValidationStatus::Fail:
      s.word = tr("FAIL");
      s.sentence = tr("The model needs fixes before export.");
      break;
    case ValidationStatus::NotValidated:
      s.word = tr("NOT CHECKED");
      s.sentence = has_model                  ? tr("Validate to check the model against your drawings.")
                   : lowpoly_can_rescue(v.doc) ? missing_views_text(v.doc)
                                               : tr("Reconstruct to build the model from your drawings.");
      break;
  }
  if (v.doc.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: the low-poly gate is the status; its reasons are the bullets.
    const nlohmann::json why = v.status.validation.is_object() ? v.status.validation : nlohmann::json::object();
    if (s.status == ValidationStatus::NotValidated)
      s.sentence = v.doc.masks.parts.empty() ? tr("Suggest or paint typed parts, then build the low-poly character.")
                                             : tr("Build the low-poly character from your typed parts.");
    else if (s.status == ValidationStatus::Pass)
      s.sentence = tr("Low-poly character ready to export (%1 / %2 triangles).")
                       .arg(why.value("triangles", 0))
                       .arg(why.value("budget", 0));
    else if (s.status == ValidationStatus::NeedsReview)
      s.sentence = plural(std::max<int>(1, static_cast<int>(why.value("blocking_conflicts", nlohmann::json::array()).size())),
                          "%1 thing needs your decision.", "%1 things need your decision.");
    for (const auto& c : why.value("blocking_conflicts", nlohmann::json::array()))
      s.bullets.push_back({qs(c.value("text", std::string())), Tone::Review});
    for (const auto& c : why.value("failed_critical_checks", nlohmann::json::array()))
      if (c.is_string()) s.bullets.push_back({check_title(qs(c.get<std::string>())), Tone::Fail});
    if (s.bullets.size() > 6) s.bullets.resize(6);
    return s;
  }
  if (s.status == ValidationStatus::NotValidated) return s;
  // Blocking issues first, grouped by kind (two holes = one bullet).
  std::map<QString, std::pair<int, const Issue*>> groups;
  std::vector<QString> order;
  for (const Issue& i : issues) {
    if (i.level != IssueLevel::Blocking) continue;
    auto [it, fresh] = groups.try_emplace(i.kind, 0, &i);
    if (fresh) order.push_back(i.kind);
    ++it->second.first;
  }
  for (const QString& k : order) {
    const auto& [count, first] = groups[k];
    QString text = first->bullet;
    if (count > 1 && k == "possible_recess") text = tr("%1 possible holes — depth unknown").arg(count);
    else if (count > 1) text = QStringLiteral("%1 (×%2)").arg(first->bullet).arg(count);
    s.bullets.push_back({text, first->tone});
  }
  // How well the model reproduces each drawing.
  if (v.doc.metrics)
    for (ViewRole r : kAllViewRoles) {
      const auto it = v.doc.metrics->views.find(r);
      if (it == v.doc.metrics->views.end()) continue;
      const double iou = it->second.iou;
      s.bullets.push_back({tr("%1 match: %2").arg(view_title(r), percent(iou)), iou >= 0.999 ? Tone::Neutral : Tone::Review});
    }
  // Estimated geometry, one line.
  int hidden = 0;
  bool surfaces = false;
  for (const Issue& i : issues) {
    if (i.tone != Tone::Info) continue;
    if (i.kind.startsWith("inferred_surface")) surfaces = true;
    else hidden += static_cast<int>(i.candidates.size());
  }
  if (hidden > 0) s.bullets.push_back({plural(hidden, "%1 hidden part estimated", "%1 hidden parts estimated"), Tone::Info});
  else if (surfaces) s.bullets.push_back({tr("Hidden surfaces estimated"), Tone::Info});
  if (s.bullets.size() > 6) s.bullets.resize(6);
  return s;
}

Guidance next_step(const SessionView& v, const std::vector<Issue>& issues) {
  Guidance g;
  const Document& d = v.doc;
  const std::vector<ViewRole> missing = missing_views(d);
  if (d.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: Front (+ optional Side/Top) → typed parts → build → export.
    bool front = false, side = false;
    for (const SourceView& s : d.views) front |= s.role == ViewRole::Front, side |= s.role == ViewRole::Side;
    if (!front) {
      g.text = tr("Drop your Front drawing (and Side, if you have it) to begin");
      g.action = "import", g.action_label = tr("Import…");
    } else if (d.masks.parts.empty()) {
      g.text = side ? tr("Drawings ready — let 3re suggest the character's parts")
                    : tr("Front only: depth will be estimated. Add a Side drawing, or suggest the parts");
      g.action = "lowpoly_suggest", g.action_label = tr("Suggest parts…"), g.tone = Tone::Accent;
    } else if (v.status.status == ValidationStatus::NotValidated) {
      g.text = tr("Parts ready — build the low-poly character");
      g.action = "lowpoly_build", g.action_label = tr("Build"), g.tone = Tone::Accent;
    } else if (v.status.status == ValidationStatus::Pass) {
      g.text = tr("Character checked — ready to export");
      g.action = "export", g.action_label = tr("Export…"), g.tone = Tone::Pass;
    } else if (v.status.status == ValidationStatus::NeedsReview) {
      g.text = tr("The character needs your review (see the Inspector)");
      g.tone = Tone::Review;
    } else {
      g.text = tr("The character fails a check (see the Inspector)");
      g.tone = Tone::Fail;
    }
    return g;
  }
  if (d.views.empty()) {
    g.text = tr("Visual Hull: drop your Front, Side and Top drawings to begin");
    g.action = "import";
    g.action_label = tr("Import…");
    return g;
  }
  if (lowpoly_can_rescue(d)) {
    // Never a dead end: the drawings the user has are enough for Low-Poly Studio (the long text is in the
    // Views panel and the Inspector).
    const bool top = std::find(missing.begin(), missing.end(), ViewRole::Top) != missing.end();
    g.text = missing.size() > 1 ? tr("Visual Hull needs Side and Top drawings")
             : top              ? tr("Visual Hull needs a Top drawing")
                                : tr("Visual Hull needs a Side drawing");
    g.action = "switch_lowpoly";
    g.action_label = tr("Switch to Low-Poly Studio (no Top needed)");
    g.tone = Tone::Review;
    return g;
  }
  if (!missing.empty()) {
    g.text = tr("Next: add the %1 drawing (%2)").arg(view_title(missing.front()), view_orientation(missing.front()));
    g.action = "import";
    g.action_label = tr("Import…");
    return g;
  }
  if (!d.solid) {
    g.text = tr("Drawings ready — build the 3D model");
    g.action = "run_all";
    g.action_label = tr("Reconstruct");
    g.tone = Tone::Accent;
    return g;
  }
  int blocking = 0;
  for (const Issue& i : issues) blocking += i.level == IssueLevel::Blocking;
  switch (v.status.status) {
    case ValidationStatus::NotValidated:
      g.text = tr("Check the model against your drawings");
      g.action = "validate";
      g.action_label = tr("Validate");
      g.tone = Tone::Accent;
      break;
    case ValidationStatus::NeedsReview:
      g.text = blocking == 1 ? tr("1 issue needs your review") : tr("%1 issues need your review").arg(blocking);
      g.action = "review";
      g.action_label = tr("Review");
      g.tone = Tone::Review;
      break;
    case ValidationStatus::Fail:
      g.text = tr("The model needs fixes before export");
      g.action = "review";
      g.action_label = tr("Review");
      g.tone = Tone::Fail;
      break;
    case ValidationStatus::Pass:
      g.text = tr("Validated — ready to export");
      g.action = "export";
      g.action_label = tr("Export…");
      g.tone = Tone::Pass;
      break;
  }
  return g;
}

}  // namespace p3dgui
