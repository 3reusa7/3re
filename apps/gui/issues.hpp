#pragma once
// What the artist needs to know, derived from the engine's conflicts and
// validation report: a short list of issues in plain language, each with the
// world regions it concerns (for highlighting in the viewport and drawings)
// and the correction the inspector offers. Also the status summary and the
// next-step guidance. Descriptive only: nothing here decides anything.
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector3D>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/job_runner.hpp"
#include "theme.hpp"
#include "view_state.hpp"

namespace p3dgui {

enum class IssueLevel { Blocking, Warning, Note };
enum class IssueFix { None, ChooseParts, SetDepth, FixDrawings };

struct Issue {
  IssueLevel level = IssueLevel::Note;
  theme::Tone tone = theme::Tone::Neutral;
  IssueFix fix = IssueFix::None;
  QString kind;         // engine vocabulary (Advanced only)
  QString title;        // plain language, e.g. "Parts don't match between views"
  QString explanation;  // one or two plain sentences
  QString bullet;       // short form for the status card, e.g. "3 parts don't match between views"
  QString engine_text;  // the engine's own sentence (Advanced only)
  int conflict_index = -1;
  std::vector<int> candidates;  // engine candidate ids
  std::vector<int> parts;       // affected part ids
  std::vector<std::vector<int>> alternatives;  // candidate sets the engine found
  std::optional<pixel3d::ViewRole> view;       // drawing the issue was found in
  QRect box;                                   // region in that drawing (aligned pixels)
  std::vector<Region3> regions;                // world boxes to highlight
  nlohmann::json raw;
  // Identity that survives a rebuild of the list (kind + candidates/box).
  QString key() const;
};

// Candidate (one possible block of a part) in artist terms.
struct CandidateInfo {
  int id = -1;
  int part = -1;
  QString status;    // engine status
  QString position;  // "Back left", "Front centre (lower)", ...
  QRect footprint;   // Top drawing pixels x0, z0, x1, z1 as a rect
  Region3 region;
  double volume = 0;
};

struct Bullet {
  QString text;
  theme::Tone tone = theme::Tone::Neutral;
};

struct StatusSummary {
  pixel3d::ValidationStatus status = pixel3d::ValidationStatus::NotValidated;
  QString word;      // PASS, NEEDS REVIEW, FAIL, NOT CHECKED
  QString sentence;  // one plain sentence
  std::vector<Bullet> bullets;  // at most six
  int blocking = 0;
};

// Next step for the hint pill and the primary toolbar button.
struct Guidance {
  QString text;
  QString action;  // action key: import, run_all, validate, review, export, or empty
  QString action_label;
  theme::Tone tone = theme::Tone::Neutral;
};

std::vector<Issue> build_issues(const pixel3d::SessionView& v);
// Moves the region of each depth issue from the whole column through the model
// to a thin plate on the surface its drawing shows (found by ray casting).
void place_on_surface(std::vector<Issue>& issues, const Scene& s, pixel3d::Dims d);
StatusSummary summarize(const pixel3d::SessionView& v, const std::vector<Issue>& issues);
Guidance next_step(const pixel3d::SessionView& v, const std::vector<Issue>& issues);

std::vector<CandidateInfo> candidate_infos(const pixel3d::Document& d, const std::vector<int>& ids);
Region3 candidate_region(const pixel3d::Document& d, int candidate);
// Position as seen from the front: footprint in Top pixels, band rows in world Y.
QString position_name(QRectF footprint_top, double y0, double y1, pixel3d::Dims d);
// Projection of a world box into a drawing (aligned image pixels, v down).
QRectF view_rect(const Region3& r, pixel3d::ViewRole view, pixel3d::Dims d);

// Plain-language vocabulary.
QString view_title(pixel3d::ViewRole r);        // "Front", "Side", "Top", "Back", "Right", "Bottom"
QString view_orientation(pixel3d::ViewRole r);  // "seen from the front", ...
QString part_status_words(const QString& status);
QString part_name(const pixel3d::Part3D& p);  // "Part 3" (1-based), or the user's own label
theme::Tone part_status_tone(const QString& status);
QString check_title(const QString& name);       // failed quality check in plain words
QString percent(double fraction);               // 0.9994 -> "99.9 %", 1 -> "100 %"

// The project type, always shown to the user: Low-Poly Studio (the default) or Visual Hull.
QString project_type_name(bool lowpoly);   // "Low-Poly Studio", "Visual Hull"
QString project_type_needs(bool lowpoly);  // "Front + Side, or Front only", "needs Front + Side + Top"
// Front, Side and Top drawings a visual-hull project still lacks (none in Low-Poly Studio or a single-drawing project).
std::vector<pixel3d::ViewRole> missing_views(const pixel3d::Document& d);
// A visual-hull project with its Front drawing but without Side or Top: Low-Poly Studio builds from what it has.
bool lowpoly_can_rescue(const pixel3d::Document& d);
// "Visual Hull needs a Top drawing (from above). …" for such a project.
QString missing_views_text(const pixel3d::Document& d);

}  // namespace p3dgui
