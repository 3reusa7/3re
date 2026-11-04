#include "inspector.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>
#include <set>

#include "pixel3d/engine/inspect.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "lowpoly_panel.hpp"
#include "qt_util.hpp"
#include "views_panel.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;
using theme::Tone;

namespace {

// A flat, full-width button whose content is a layout (icon, texts, trailing).
class RowButton : public QPushButton {
 public:
  using QPushButton::QPushButton;
  QSize sizeHint() const override { return layout() ? layout()->sizeHint() : QPushButton::sizeHint(); }
  QSize minimumSizeHint() const override { return layout() ? layout()->minimumSize() : QPushButton::minimumSizeHint(); }
  bool hasHeightForWidth() const override { return layout() && layout()->hasHeightForWidth(); }
  int heightForWidth(int w) const override { return layout() ? layout()->totalHeightForWidth(w) : -1; }
};

Icon tone_icon(Tone t) {
  switch (t) {
    case Tone::Pass: return Icon::StatusPass;
    case Tone::Review: return Icon::StatusReview;
    case Tone::Fail: return Icon::StatusFail;
    case Tone::Info:
    case Tone::Neutral:
    case Tone::Accent: break;
  }
  return Icon::Info;
}

QLabel* icon_label(const QIcon& ic, int px = theme::icon_size::sm) {
  auto* l = new QLabel;
  l->setPixmap(ic.pixmap(QSize(px, px)));
  l->setFixedSize(px, px);
  return l;
}

QPushButton* make_row(const QIcon& ic, const QString& title, const QString& subtitle, const QString& tooltip = {}) {
  auto* b = new RowButton;
  b->setProperty("kind", "row");
  b->setCursor(Qt::PointingHandCursor);
  b->setToolTip(tooltip);
  auto* h = new QHBoxLayout(b);
  h->setContentsMargins(theme::space::sm, theme::space::sm, theme::space::sm, theme::space::sm);
  h->setSpacing(theme::space::sm + 2);
  auto* il = icon_label(ic);
  h->addWidget(il, 0, Qt::AlignTop);
  auto* texts = new QVBoxLayout;
  texts->setSpacing(2);
  auto* t = new QLabel(title);
  t->setWordWrap(true);
  texts->addWidget(t);
  if (!subtitle.isEmpty()) {
    auto* s = make_label(subtitle, "small");
    s->setWordWrap(true);
    texts->addWidget(s);
  }
  h->addLayout(texts, 1);
  h->addWidget(icon_label(icon(Icon::ChevronRight), 14), 0, Qt::AlignVCenter);
  for (QLabel* l : b->findChildren<QLabel*>()) l->setAttribute(Qt::WA_TransparentForMouseEvents);
  return b;
}

QString bullet_html(const Bullet& b) {
  const QColor dot = b.tone == Tone::Neutral ? theme::colors().text_muted : theme::tone_color(b.tone);
  return QStringLiteral("<span style='color:%1'>●</span>&nbsp;&nbsp;%2").arg(dot.name(), b.text.toHtmlEscaped());
}

// Key/value facts in two columns (muted keys).
QWidget* facts(const std::vector<std::pair<QString, QString>>& rows) {
  auto* w = new QWidget;
  auto* g = new QGridLayout(w);
  g->setContentsMargins(0, 0, 0, 0);
  g->setHorizontalSpacing(theme::space::md);
  g->setVerticalSpacing(theme::space::xs + 2);
  int r = 0;
  for (const auto& [k, v] : rows) {
    auto* kl = make_label(k, "muted");
    auto* vl = new QLabel(v);
    vl->setWordWrap(true);
    vl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    g->addWidget(kl, r, 0, Qt::AlignTop);
    g->addWidget(vl, r, 1);
    ++r;
  }
  g->setColumnStretch(1, 1);
  return w;
}

QString size_text(const Region3& r) {
  const QVector3D s = r.hi - r.lo;
  return QObject::tr("%1 × %2 × %3 px").arg(std::lround(s.x())).arg(std::lround(s.y())).arg(std::lround(s.z()));
}

QString facing(const vec3d& n) {
  const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
  if (az >= ax && az >= ay && az > 0.9) return n.z > 0 ? QObject::tr("Faces the front") : QObject::tr("Faces the back");
  if (ax >= ay && ax > 0.9) return n.x > 0 ? QObject::tr("Faces right") : QObject::tr("Faces left");
  if (ay > 0.9) return n.y > 0 ? QObject::tr("Faces up") : QObject::tr("Faces down");
  return QObject::tr("Slanted face");
}

// Mirror button with a menu: in place or as a copy, across X, Y or Z.
QPushButton* mirror_button(const std::function<void(const QString&)>& emit_action) {
  auto* b = make_button(QObject::tr("Mirror"), nullptr, icon(Icon::Mirror));
  auto* menu = new QMenu(b);
  for (int axis = 0; axis < 3; ++axis) {
    const QString name = QStringLiteral("XYZ").mid(axis, 1);
    menu->addAction(QObject::tr("Mirror copy across %1").arg(name), [emit_action, axis] {
      emit_action(QStringLiteral("part_mirror_copy_%1").arg(axis));
    });
  }
  menu->addSeparator();
  for (int axis = 0; axis < 3; ++axis) {
    const QString name = QStringLiteral("XYZ").mid(axis, 1);
    menu->addAction(QObject::tr("Flip in place across %1").arg(name), [emit_action, axis] {
      emit_action(QStringLiteral("part_mirror_%1").arg(axis));
    });
  }
  b->setMenu(menu);
  return b;
}

// The model's parts (reconstructed, created by edits, defined by masks), as inspect reports them.
std::vector<nlohmann::json> model_parts(const Document& d) {
  std::vector<nlohmann::json> out;
  for (const auto& p : parts_json(d)) out.push_back(p);
  return out;
}

Region3 part_bounds(const Document& d, int part) {
  std::vector<Region3> rs;
  if (!d.solid) return {};
  const Mesh& m = d.solid->mesh;
  for (size_t t = 0; t < m.tris.size() && t < m.tri_part.size(); ++t) {
    if (m.tri_part[t] != part) continue;
    for (uint32_t i : m.tris[t]) {
      const QVector3D q(static_cast<float>(m.positions[i].x), static_cast<float>(m.positions[i].y),
                        static_cast<float>(m.positions[i].z));
      rs.push_back({q, q});
    }
  }
  return bounds(rs);
}

QString model_part_name(const Document& d, int id) {
  if (const auto it = d.part_names.find(id); it != d.part_names.end()) return qs(it->second);
  if (d.correspondence)
    for (const Part3D& p : d.correspondence->parts)
      if (p.id == id) return part_name(p);
  return id >= kFirstUserPart ? QObject::tr("Your part %1").arg(id - kFirstUserPart + 1) : QObject::tr("Part %1").arg(id + 1);
}

}  // namespace

// ---- parts chooser ---------------------------------------------------------------

PartsChooser::PartsChooser(const Issue& issue, const SessionView& v, bool advanced, QWidget* parent) : QWidget(parent) {
  alternatives_ = issue.alternatives;
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::sm);
  lay->addWidget(make_label(tr("Which parts are real?"), "section"));
  if (alternatives_.size() > 1) {
    lay->addWidget(make_label(tr("Arrangements that fit the drawings:"), "small"));
    auto* grid = new QGridLayout;
    grid->setSpacing(theme::space::xs);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    for (size_t i = 0; i < alternatives_.size(); ++i) {
      auto* b = make_button(tr("Option %1 · %2 parts").arg(i + 1).arg(alternatives_[i].size()), "choice");
      b->setToolTip(tr("Keep exactly these parts (hover to see them outlined)"));
      b->setCheckable(true);
      group->addButton(b);
      grid->addWidget(b, static_cast<int>(i / 2), static_cast<int>(i % 2));
      arrangement_buttons_.push_back(b);
      connect(b, &QPushButton::clicked, this, [this, i] { chooseArrangement(static_cast<int>(i)); });
      b->installEventFilter(this);
      b->setProperty("arrangement", static_cast<int>(i));
    }
    lay->addLayout(grid);
  }
  const std::vector<CandidateInfo> infos = candidate_infos(v.doc, issue.candidates);
  for (const CandidateInfo& ci : infos) {
    QString text = tr("%1 part").arg(ci.position);
    if (advanced) text += tr("  · #%1").arg(ci.id);
    auto* cb = new QCheckBox(text);
    cb->setChecked(true);
    cb->setCursor(Qt::PointingHandCursor);
    cb->setToolTip(tr("Keep this part (unchecked = remove it). Hover to see it outlined in the model and the drawings."));
    cb->setProperty("candidate", ci.id);
    cb->installEventFilter(this);
    lay->addWidget(cb);
    rows_.push_back({ci.id, cb});
  }
  auto* foot = new QHBoxLayout;
  apply_ = make_button(tr("Apply choice"), "primary");
  foot->addWidget(apply_);
  foot->addStretch();
  lay->addLayout(foot);
  auto* note = make_label(tr("Only decide what you know — a Back or Right view can settle this too. Choices the drawings "
                             "can't support are refused."),
                          "small");
  note->setWordWrap(true);
  lay->addWidget(note);
  connect(apply_, &QPushButton::clicked, this, [this] { emit apply(decisions()); });
}

bool PartsChooser::eventFilter(QObject* o, QEvent* e) {
  if (e->type() == QEvent::Enter) {
    if (o->property("candidate").isValid()) emit candidatesHovered({o->property("candidate").toInt()});
    const QVariant a = o->property("arrangement");
    if (a.isValid() && a.toInt() >= 0 && a.toInt() < static_cast<int>(alternatives_.size()))
      emit candidatesHovered(alternatives_[static_cast<size_t>(a.toInt())]);
  } else if (e->type() == QEvent::Leave) {
    emit candidatesHovered({});
  }
  return QWidget::eventFilter(o, e);
}

void PartsChooser::chooseArrangement(int index) {
  if (index < 0 || index >= static_cast<int>(alternatives_.size())) return;
  const std::set<int> keep(alternatives_[static_cast<size_t>(index)].begin(), alternatives_[static_cast<size_t>(index)].end());
  for (auto& [id, cb] : rows_) cb->setChecked(keep.contains(id));
  if (index < static_cast<int>(arrangement_buttons_.size())) arrangement_buttons_[static_cast<size_t>(index)]->setChecked(true);
}

std::vector<UserDecision> PartsChooser::decisions() const {
  std::vector<UserDecision> d;
  for (const auto& [id, cb] : rows_) d.push_back({id, cb->isChecked()});
  return d;
}

// ---- depth editor -----------------------------------------------------------------

DepthEditor::DepthEditor(const Issue& issue, const SessionView& v, QWidget* parent) : QWidget(parent), issue_(issue) {
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::sm);
  const ViewRole role = issue.view.value_or(ViewRole::Top);
  // The area in its drawing, zoomed, outlined.
  const DrawingInfo info = drawing_info(v, role);
  if (info.assigned && !issue.box.isNull()) {
    const QRect crop = issue.box.adjusted(-5, -5, 5, 5).intersected(info.image.rect());
    const QImage part = info.image.copy(crop);
    QPixmap pm(QSize(260, 132) * devicePixelRatioF());
    pm.setDevicePixelRatio(devicePixelRatioF());
    pm.fill(Qt::transparent);
    {
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      const QRectF area(0, 0, 260, 132);
      p.setPen(Qt::NoPen);
      p.setBrush(theme::colors().preview_bg);
      p.drawRoundedRect(area, theme::radius::md, theme::radius::md);
      const QRectF r = paint_pixel_image(p, part, area.adjusted(8, 8, -8, -8), 24);
      const double z = r.width() / std::max(1, part.width());
      const QRect b = issue.box.translated(-crop.topLeft());
      p.setPen(QPen(theme::colors().review, 2));
      p.setBrush(theme::with_alpha(theme::colors().review, 0.12));
      p.drawRect(QRectF(r.x() + b.x() * z - 1, r.y() + b.y() * z - 1, b.width() * z + 2, b.height() * z + 2));
    }
    auto* img = new QLabel;
    img->setPixmap(pm);
    lay->addWidget(img);
  }
  lay->addWidget(make_label(tr("What is this area?"), "section"));
  hole_ = new QRadioButton(tr("A hole or cavity, starting"));
  flush_ = new QRadioButton(tr("Painted detail — keep it flat"));
  hole_->setChecked(true);
  double extent = 64;
  if (v.doc.align) extent = view_depth_extent(role, v.doc.align->dims);
  depth_ = new QDoubleSpinBox;
  depth_->setRange(0, extent);
  depth_->setDecimals(1);
  depth_->setValue(std::min(extent, 1.0));
  depth_->setSuffix(tr(" px in"));
  depth_->setMinimumWidth(104);
  depth_->setToolTip(tr("How far below the %1 surface the material begins").arg(view_title(role)));
  auto* row = new QHBoxLayout;
  row->addWidget(hole_);
  row->addWidget(depth_);
  row->addStretch();
  lay->addLayout(row);
  lay->addWidget(flush_);
  connect(flush_, &QRadioButton::toggled, depth_, [this](bool f) { depth_->setEnabled(!f); });
  auto* foot = new QHBoxLayout;
  apply_ = make_button(tr("Apply depth"), "primary");
  foot->addWidget(apply_);
  foot->addStretch();
  lay->addLayout(foot);
  connect(apply_, &QPushButton::clicked, this, [this] {
    if (auto c = constraint()) emit apply(*c);
  });
}

void DepthEditor::setDepth(double px) {
  hole_->setChecked(true);
  depth_->setValue(px);
}

void DepthEditor::setFlush(bool flush) { (flush ? flush_ : hole_)->setChecked(true); }

std::optional<DepthConstraint> DepthEditor::constraint() const {
  if (!issue_.view || issue_.box.isNull()) return std::nullopt;
  const QRect& b = issue_.box;
  DepthConstraint c;
  c.view = *issue_.view;
  c.region = rect_polys(b.left(), b.top(), b.left() + b.width(), b.top() + b.height());
  c.depth_from = flush_->isChecked() ? 0 : depth_->value();
  c.depth_to = 1e9;
  c.label = flush_->isChecked() ? "flush" : "recess";
  return c;
}

// ---- inspector ----------------------------------------------------------------------

Inspector::Inspector(QWidget* parent) : QWidget(parent) {
  setProperty("surface", "panel");
  setAttribute(Qt::WA_StyledBackground, true);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* content = new QWidget;
  root_ = new QVBoxLayout(content);
  root_->setContentsMargins(theme::space::lg, theme::space::xs, theme::space::lg, theme::space::lg);
  root_->setSpacing(theme::space::lg);
  status_host_ = new QWidget;
  status_layout_ = new QVBoxLayout(status_host_);
  status_layout_->setContentsMargins(0, 0, 0, 0);
  root_->addWidget(status_host_);
  message_ = new QLabel;
  message_->setWordWrap(true);
  message_->hide();
  root_->addWidget(message_);
  root_->addStretch(1);
  scroll->setWidget(content);
  outer->addWidget(scroll);
  rebuildPage();
}

QString Inspector::statusText() const { return qs(to_string(view_.status.status)); }

QString Inspector::statusWord() const { return status_word_ ? status_word_->text() : QString(); }

void Inspector::setAdvanced(bool on) {
  if (advanced_ == on) return;
  advanced_ = on;
  rebuildPage();
}

void Inspector::update_from(const SessionView& v, const std::vector<Issue>& issues) {
  view_ = v;
  issues_ = issues;
  // Stay on the same page when it still exists.
  if (page_ == InspectorPage::Issue) {
    bool found = false;
    for (const Issue& i : issues_) found |= i.key() == issue_key_;
    if (!found) page_ = InspectorPage::Overview;
  } else if (page_ == InspectorPage::Part) {
    bool found = false;
    for (const auto& p : model_parts(v.doc)) found |= p.value("id", -2) == part_;
    if (!found) page_ = InspectorPage::Overview;
  } else if (page_ == InspectorPage::Face && v.doc.revision != face_revision_) {
    page_ = InspectorPage::Overview;
  }
  rebuildPage();
}

void Inspector::showOverview() {
  page_ = InspectorPage::Overview;
  message_->hide();
  rebuildPage();
}

bool Inspector::showIssue(const QString& key) {
  for (const Issue& i : issues_)
    if (i.key() == key) {
      page_ = InspectorPage::Issue;
      issue_key_ = key;
      message_->hide();
      rebuildPage();
      return true;
    }
  return false;
}

void Inspector::showPart(int part) {
  page_ = InspectorPage::Part;
  part_ = part;
  message_->hide();
  rebuildPage();
}

void Inspector::showFace(int triangle) {
  page_ = InspectorPage::Face;
  face_ = triangle;
  face_revision_ = view_.doc.revision;
  message_->hide();
  rebuildPage();
}

void Inspector::showMessage(const QString& text, Tone tone) {
  message_->setText(text);
  message_->setProperty("type", "secondary");
  theme::set_prop(message_, "tone", theme::tone_name(tone));
  message_->setVisible(!text.isEmpty());
}

Section* Inspector::advancedSection(const QString& title) { return new Section(title, nullptr, advanced_); }

void Inspector::rebuildStatus() {
  while (QLayoutItem* it = status_layout_->takeAt(0)) {
    if (QWidget* w = it->widget()) {
      w->hide();
      w->deleteLater();
    }
    delete it;
  }
  status_word_ = nullptr;
  const StatusSummary s = summarize(view_, issues_);
  const Tone tone = theme::tone_for(s.status);
  const bool compact = page_ != InspectorPage::Overview;  // detail pages keep their tool above the fold
  auto* card = new Card;
  card->setTone(tone);
  auto* lay = new QVBoxLayout(card);
  lay->setContentsMargins(theme::space::lg + 2, theme::space::md + 2, theme::space::lg, theme::space::lg);
  lay->setSpacing(theme::space::sm);
  auto* head = new QHBoxLayout;
  head->setSpacing(theme::space::sm + 2);
  head->addWidget(icon_label(status_icon(s.status), 24));
  status_word_ = make_label(s.word, "status");
  status_word_->setProperty("tone", theme::tone_name(tone));
  head->addWidget(status_word_, 1);
  lay->addLayout(head);
  auto* sentence = make_label(s.sentence, "secondary");
  sentence->setWordWrap(true);
  lay->addWidget(sentence);
  if (compact) {
    lay->setContentsMargins(theme::space::lg + 2, theme::space::md, theme::space::lg, theme::space::md);
    status_layout_->addWidget(card);
    status_host_->setVisible(!view_.doc.views.empty());
    return;
  }
  if (!s.bullets.empty()) {
    QStringList lines;
    for (const Bullet& b : s.bullets) lines << bullet_html(b);
    auto* bl = new QLabel(lines.join(QStringLiteral("<br>")));
    bl->setTextFormat(Qt::RichText);
    bl->setWordWrap(true);
    lay->addSpacing(theme::space::xs);
    lay->addWidget(bl);
  }
  // One button: the next thing to do.
  QString label, action;
  const bool required = missing_views(view_.doc).empty();
  const bool lowpoly = view_.doc.settings.lowpoly.enabled;
  if (lowpoly) {
    // LOW-POLY STUDIO: build the character, then export it.
    if (s.status == ValidationStatus::NotValidated || s.status == ValidationStatus::Fail)
      label = view_.doc.masks.parts.empty() ? tr("Suggest parts…") : tr("Build character"),
      action = view_.doc.masks.parts.empty() ? "lowpoly_suggest" : "lowpoly_build";
    else
      label = tr("Export…"), action = "export";
  } else switch (s.status) {
    case ValidationStatus::Pass: label = tr("Export…"), action = "export"; break;
    case ValidationStatus::NeedsReview: label = tr("Review issues"), action = "review"; break;
    case ValidationStatus::Fail: label = tr("Review problems"), action = "review"; break;
    case ValidationStatus::NotValidated:
      if (view_.doc.solid) label = tr("Validate"), action = "validate";
      else if (required) label = tr("Reconstruct"), action = "run_all";
      else if (lowpoly_can_rescue(view_.doc)) label = tr("Switch to Low-Poly Studio"), action = "switch_lowpoly";
      else label = tr("Import drawings…"), action = "import";
      break;
  }
  auto* primary = make_button(label, "primary");
  primary->setObjectName("statusAction");
  primary->setProperty("action", action);
  lay->addSpacing(theme::space::xs);
  lay->addWidget(primary);
  connect(primary, &QPushButton::clicked, this, [this, action] { emit actionRequested(action); });
  // Advanced: every check and the raw metrics.
  if (view_.doc.gate && s.status != ValidationStatus::NotValidated) {
    auto* adv = advancedSection(tr("Advanced details"));
    auto* table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({tr("Check"), tr("Value"), tr("Threshold"), tr("Result")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFont(theme::font(theme::Type::Small));
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->verticalHeader()->setDefaultSectionSize(22);
    for (const Check& c : view_.doc.gate->checks) {
      const int r = table->rowCount();
      table->insertRow(r);
      const QString cells[] = {qs(c.name), QString::number(c.value, 'g', 4), QString::number(c.threshold, 'g', 4),
                               c.passed ? tr("pass") : c.critical ? tr("FAIL") : tr("fail")};
      for (int k = 0; k < 4; ++k) {
        auto* item = new QTableWidgetItem(cells[k]);
        item->setToolTip(qs(c.text));
        if (!c.passed) item->setForeground(c.critical ? theme::colors().fail : theme::colors().review);
        table->setItem(r, k, item);
      }
    }
    table->setFixedHeight(table->horizontalHeader()->height() + 22 * std::min(table->rowCount(), 12) + 4);
    adv->body()->addWidget(table);
    if (view_.doc.metrics) {
      const QualityMetrics& m = *view_.doc.metrics;
      QStringList t;
      for (const auto& [r, vm] : m.views)
        t << tr("%1: shape %2, colour %3, %4 px differ")
                 .arg(view_title(r), percent(vm.iou), percent(vm.color_exact_fraction))
                 .arg(vm.mismatch_px);
      for (const RotationMetric& r : m.rotations) t << tr("turned %1°: shape %2").arg(r.yaw).arg(percent(r.iou));
      t << tr("thickness %1 · turntable min area %2").arg(m.thickness_ratio, 0, 'f', 2).arg(m.turntable.min_area_ratio, 0, 'f', 2);
      t << tr("triangles %1 · estimated volume %2 · undecided volume %3")
               .arg(m.topology.triangle_count)
               .arg(percent(m.inferred_volume_fraction))
               .arg(percent(m.ambiguous_volume_fraction));
      t << tr("engine status %1 · profile %2").arg(statusText(), qs(to_string(view_.doc.gate->profile)));
      auto* ml = make_label(t.join('\n'), "small");
      ml->setWordWrap(true);
      ml->setTextInteractionFlags(Qt::TextSelectableByMouse);
      adv->body()->addWidget(ml);
    }
    lay->addSpacing(theme::space::xs);
    lay->addWidget(adv);
  }
  status_layout_->addWidget(card);
  status_host_->setVisible(!view_.doc.views.empty());
}

void Inspector::setPage(QWidget* w) {
  if (page_widget_) {
    page_widget_->hide();
    page_widget_->deleteLater();
  }
  page_widget_ = w;
  root_->insertWidget(root_->indexOf(message_) + 1, w);
}

void Inspector::rebuildPage() {
  rebuildStatus();
  chooser_ = nullptr;
  depth_editor_ = nullptr;
  QWidget* w = nullptr;
  if (page_ == InspectorPage::Issue) {
    for (const Issue& i : issues_)
      if (i.key() == issue_key_) w = buildIssue(i);
    if (!w) page_ = InspectorPage::Overview;
  } else if (page_ == InspectorPage::Part) {
    w = buildPart(part_);
  } else if (page_ == InspectorPage::Face) {
    w = buildFace(face_);
  }
  if (!w) w = buildOverview();
  setPage(w);
}

QWidget* Inspector::navRow(const QString& back_text, QWidget* trailing) {
  auto* w = new QWidget;
  auto* h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  auto* back = make_button(back_text, "link", icon(Icon::ChevronLeft));
  back->setIconSize(QSize(14, 14));
  connect(back, &QPushButton::clicked, this, &Inspector::overviewRequested);
  h->addWidget(back);
  h->addStretch();
  if (trailing) h->addWidget(trailing);
  return w;
}

QWidget* Inspector::buildOverview() {
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::lg);
  const Document& d = view_.doc;
  if (d.views.empty()) {
    auto* hint = make_label(tr("Import Front, Side and Top drawings to begin. The model's status, its parts and anything "
                               "that needs your decision will appear here."),
                            "secondary");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    return w;
  }
  if (d.settings.lowpoly.enabled) {
    auto* panel = new LowpolyPanel(view_, lowpoly_pose_);
    connect(panel, &LowpolyPanel::suggestRequested, this, &Inspector::lowpolySuggestRequested);
    connect(panel, &LowpolyPanel::layerPartsRequested, this, &Inspector::lowpolyLayerPartsRequested);
    connect(panel, &LowpolyPanel::buildRequested, this, &Inspector::lowpolyBuildRequested);
    connect(panel, &LowpolyPanel::partTypeRequested, this, &Inspector::lowpolyPartTypeRequested);
    connect(panel, &LowpolyPanel::poseToggled, this, &Inspector::lowpolyPoseToggled);
    connect(panel, &LowpolyPanel::exportRequested, this, [this] { emit actionRequested("export"); });
    lay->addWidget(panel);
  }
  if (!issues_.empty()) {
    int review = 0;
    for (const Issue& i : issues_) review += i.level == IssueLevel::Blocking;
    auto* sec = new Section(review > 0 ? tr("Needs your review · %1").arg(review) : tr("Notes"));
    auto* notes = review > 0 ? new Section(tr("Notes"), nullptr, false) : nullptr;
    for (const Issue& i : issues_) {
      QString sub;
      if (i.view) sub = tr("%1 drawing").arg(view_title(*i.view));
      if (!i.candidates.empty())
        sub = (sub.isEmpty() ? QString() : sub + QStringLiteral(" · ")) +
              (i.candidates.size() == 1 ? tr("1 part") : tr("%1 parts").arg(i.candidates.size()));
      auto* row = make_row(icon(tone_icon(i.tone), IconStyle::Toned, i.tone), i.title, sub, i.explanation);
      const QString key = i.key();
      connect(row, &QPushButton::clicked, this, [this, key] { emit issueRequested(key); });
      if (notes && i.level != IssueLevel::Blocking) notes->body()->addWidget(row);
      else sec->body()->addWidget(row);
    }
    lay->addWidget(sec);
    if (notes) lay->addWidget(notes);
  }
  const std::vector<nlohmann::json> parts = model_parts(d);
  if (!parts.empty()) {
    auto* sec = new Section(tr("Parts · %1").arg(parts.size()), nullptr, parts.size() <= 12);
    for (const auto& pj : parts) {
      const int id = pj.value("id", -1);
      const QString st = qs(pj.value("status", std::string()));
      QString name = model_part_name(d, id);
      Region3 r;
      if (d.correspondence)
        for (const Part3D& p : d.correspondence->parts)
          if (p.id == id) {
            std::vector<Region3> rs;
            for (int c : p.candidates) rs.push_back(candidate_region(d, c));
            r = bounds(rs);
          }
      if (r.empty()) r = part_bounds(d, id);
      if (d.align && !r.empty())
        name = tr("%1 · %2").arg(name, position_name(QRectF(QPointF(r.lo.x(), r.lo.z()), QPointF(r.hi.x(), r.hi.z())),
                                                     r.lo.y(), r.hi.y(), d.align->dims));
      const bool user = pj.value("user", false);
      const Tone t = user ? Tone::Accent : part_status_tone(st);
      const QString words = user ? (pj.value("define", false) ? tr("Your part — its masks define it")
                                                              : pj.value("has_geometry", false) ? tr("Your part")
                                                                                                : tr("Your part — paint its masks"))
                                 : part_status_words(st);
      auto* row = make_row(icon(t == Tone::Pass ? Icon::StatusPass : user ? Icon::Parts : tone_icon(t), IconStyle::Toned, t),
                           name, words);
      connect(row, &QPushButton::clicked, this, [this, id] { emit partRequested(id); });
      sec->body()->addWidget(row);
    }
    auto* add = new QHBoxLayout;
    auto* np = make_button(tr("New part…"), "ghost", icon(Icon::Parts));
    np->setToolTip(tr("A part you paint on two drawings (for example one leg on Front and Side)"));
    connect(np, &QPushButton::clicked, this, [this] { emit actionRequested("part_new_define"); });
    add->addWidget(np);
    add->addStretch();
    sec->body()->addLayout(add);
    lay->addWidget(sec);
  }
  // SINGLE VIEW: the depth the user chose (or has not chosen yet).
  if (d.single_view.enabled) {
    auto* sec = new Section(tr("Single drawing · depth"));
    auto* note = make_label(d.single_view.user_depth
                                ? tr("Depth chosen by you. It is still estimated: one drawing does not show the other sides.")
                                : tr("Everything the drawing does not show is estimated. Choose the depth you intend — "
                                     "the model needs review until you do."),
                            "small");
    note->setWordWrap(true);
    if (!d.single_view.user_depth) note->setProperty("tone", "review");
    sec->body()->addWidget(note);
    auto* preset = new QComboBox;
    for (const auto& [key, text] : {std::pair{"flat", tr("Flat (1 px)")}, std::pair{"thin", tr("Thin (1/4 of the smaller side)")},
                                    std::pair{"medium", tr("Medium (1/2)")}, std::pair{"thick", tr("Thick (= smaller side)")},
                                    std::pair{"custom", tr("Custom")}})
      preset->addItem(text, QString::fromLatin1(key));
    preset->setCurrentIndex(std::max(0, preset->findData(qs(d.single_view.preset))));
    auto* depth = new QDoubleSpinBox;
    depth->setRange(0.25, 4096);
    depth->setDecimals(2);
    depth->setSuffix(tr(" px"));
    depth->setValue(d.single_view.preset == "custom" ? d.single_view.depth : 4.0);
    depth->setEnabled(d.single_view.preset == "custom");
    connect(preset, &QComboBox::currentIndexChanged, depth, [preset, depth] { depth->setEnabled(preset->currentData().toString() == "custom"); });
    auto* row = new QHBoxLayout;
    row->addWidget(preset, 1);
    row->addWidget(depth);
    sec->body()->addLayout(row);
    auto* apply = make_button(tr("Apply depth"), d.single_view.user_depth ? nullptr : "primary");
    apply->setObjectName("singleViewApply");
    connect(apply, &QPushButton::clicked, this, [this, preset, depth] {
      const std::string p = utf8(preset->currentData().toString());
      emit singleViewDepthRequested(p, p == "custom" ? std::optional<double>(depth->value()) : std::nullopt);
    });
    auto* arow = new QHBoxLayout;
    arow->addWidget(apply);
    arow->addStretch();
    sec->body()->addLayout(arow);
    lay->addWidget(sec);
  }
  auto* proj = new Section(tr("Project"));
  std::vector<std::pair<QString, QString>> rows{{tr("Name"), qs(d.settings.name)}};
  if (d.align) {
    const Dims dm = d.align->dims;
    rows.push_back({tr("Size"), tr("%1 × %2 × %3 px").arg(dm.W).arg(dm.H).arg(dm.D)});
    const double ppm = d.settings.pixels_per_meter;
    rows.push_back({tr("Real size"), tr("%1 × %2 × %3 m").arg(dm.W / ppm, 0, 'f', 2).arg(dm.H / ppm, 0, 'f', 2).arg(dm.D / ppm, 0, 'f', 2)});
  }
  rows.push_back({tr("Scale"), tr("%1 px per metre").arg(d.settings.pixels_per_meter)});
  rows.push_back({tr("Art style"), d.settings.profile == ToleranceProfile::Hand ? tr("Hand-drawn (tolerant)")
                                                                                : tr("Exact (strict)")});
  proj->body()->addWidget(facts(rows));
  auto* settings = make_button(tr("Project settings…"), "ghost", icon(Icon::Settings));
  connect(settings, &QPushButton::clicked, this, [this] { emit actionRequested("settings"); });
  auto* srow = new QHBoxLayout;
  srow->addWidget(settings);
  srow->addStretch();
  proj->body()->addLayout(srow);
  lay->addWidget(proj);
  auto* adv = advancedSection(tr("Advanced"));
  std::vector<std::pair<QString, QString>> arows{{tr("Revision"), QString::number(d.revision)},
                                                 {tr("Engine status"), statusText()},
                                                 {tr("Mode"), qs(to_string(d.mode))}};
  if (d.solid) {
    arows.push_back({tr("Triangles"), QString::number(d.solid->topology.triangle_count)});
    arows.push_back({tr("Watertight"), d.solid->topology.watertight ? tr("yes") : tr("no")});
    arows.push_back({tr("Shells"), QString::number(d.solid->topology.shells)});
  }
  arows.push_back({tr("Textured"), d.texture ? tr("yes") : tr("no")});
  arows.push_back({tr("Decisions"), tr("%1 part, %2 depth").arg(d.decisions.size()).arg(d.depth_constraints.size())});
  if (!renderer_.isEmpty()) arows.push_back({tr("Renderer"), renderer_});
  adv->body()->addWidget(facts(arows));
  lay->addWidget(adv);
  return w;
}

QWidget* Inspector::buildIssue(const Issue& is) {
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::md);
  // Navigation between issues.
  int index = -1, count = 0;
  QString prev, next;
  {
    std::vector<QString> keys;
    for (const Issue& i : issues_) keys.push_back(i.key());
    count = static_cast<int>(keys.size());
    for (int k = 0; k < count; ++k)
      if (keys[static_cast<size_t>(k)] == is.key()) index = k;
    if (index > 0) prev = keys[static_cast<size_t>(index - 1)];
    if (index >= 0 && index + 1 < count) next = keys[static_cast<size_t>(index + 1)];
  }
  auto* nav = new QWidget;
  auto* nh = new QHBoxLayout(nav);
  nh->setContentsMargins(0, 0, 0, 0);
  nh->setSpacing(2);
  nh->addWidget(make_label(tr("%1 of %2").arg(index + 1).arg(count), "small"));
  for (const auto& [ic, key, tip] : {std::tuple{Icon::ChevronLeft, prev, tr("Previous issue")},
                                     std::tuple{Icon::ChevronRight, next, tr("Next issue")}}) {
    auto* b = new QToolButton;
    b->setIcon(icon(ic));
    b->setIconSize(QSize(14, 14));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setEnabled(!key.isEmpty());
    const QString k = key;
    connect(b, &QToolButton::clicked, this, [this, k] { emit issueRequested(k); });
    nh->addWidget(b);
  }
  lay->addWidget(navRow(tr("All issues"), nav));

  auto* head = new QHBoxLayout;
  head->setSpacing(theme::space::sm + 2);
  head->addWidget(icon_label(icon(tone_icon(is.tone), IconStyle::Toned, is.tone), 20), 0, Qt::AlignTop);
  auto* title = make_label(is.title, "title");
  title->setWordWrap(true);
  head->addWidget(title, 1);
  lay->addLayout(head);
  auto* expl = make_label(is.explanation, "secondary");
  expl->setWordWrap(true);
  lay->addWidget(expl);
  if (is.view) {
    auto* chips = new QHBoxLayout;
    chips->addWidget(make_chip(tr("%1 drawing").arg(view_title(*is.view)), Tone::Neutral));
    chips->addStretch();
    lay->addLayout(chips);
  }
  lay->addSpacing(theme::space::xs);
  switch (is.fix) {
    case IssueFix::ChooseParts: {
      chooser_ = new PartsChooser(is, view_, advanced_);
      connect(chooser_, &PartsChooser::apply, this, &Inspector::applyParts);
      connect(chooser_, &PartsChooser::candidatesHovered, this, &Inspector::candidatesHovered);
      lay->addWidget(chooser_);
      break;
    }
    case IssueFix::SetDepth: {
      depth_editor_ = new DepthEditor(is, view_);
      connect(depth_editor_, &DepthEditor::apply, this, &Inspector::applyDepth);
      lay->addWidget(depth_editor_);
      break;
    }
    case IssueFix::FixDrawings: {
      auto* t = make_label(tr("This is fixed in the drawings: correct them in your art tool and import them again, or "
                              "add another view."),
                           "small");
      t->setWordWrap(true);
      lay->addWidget(t);
      auto* row = new QHBoxLayout;
      auto* b = make_button(tr("Import a drawing…"), nullptr, icon(Icon::Import));
      connect(b, &QPushButton::clicked, this, [this] { emit actionRequested("import"); });
      row->addWidget(b);
      row->addStretch();
      lay->addLayout(row);
      break;
    }
    case IssueFix::None: break;
  }
  auto* frame_row = new QHBoxLayout;
  if (!is.regions.empty()) {
    auto* frame = make_button(tr("Frame in view"), "ghost", icon(Icon::Frame));
    connect(frame, &QPushButton::clicked, this, &Inspector::frameRequested);
    frame_row->addWidget(frame);
  }
  frame_row->addStretch();
  lay->addLayout(frame_row);
  auto* adv = advancedSection(tr("Technical details"));
  std::vector<std::pair<QString, QString>> rows{{tr("Kind"), is.kind}};
  if (!is.engine_text.isEmpty()) rows.push_back({tr("Engine"), is.engine_text});
  if (!is.candidates.empty()) {
    QStringList ids;
    for (int c : is.candidates) ids << QString::number(c);
    rows.push_back({tr("Candidates"), ids.join(", ")});
  }
  adv->body()->addWidget(facts(rows));
  auto* raw = new QPlainTextEdit(json_text(is.raw));
  raw->setReadOnly(true);
  raw->setFont(theme::font(theme::Type::Mono));
  raw->setMaximumHeight(150);
  adv->body()->addWidget(raw);
  lay->addWidget(adv);
  return w;
}

QWidget* Inspector::buildPart(int part) {
  const Document& d = view_.doc;
  const Part3D* p = nullptr;
  if (d.correspondence)
    for (const Part3D& q : d.correspondence->parts)
      if (q.id == part) p = &q;
  nlohmann::json pj;
  for (const auto& m : model_parts(d))
    if (m.value("id", -2) == part) pj = m;
  if (!p && pj.is_null()) return nullptr;
  auto emit_action = [this](const QString& k) { emit actionRequested(k); };
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::md);
  lay->addWidget(navRow(tr("Overview")));
  const bool user = part >= kFirstUserPart;
  const QString st = p ? qs(to_string(p->status)) : QString();
  const Tone tone = user ? Tone::Accent : part_status_tone(st);
  auto* head = new QHBoxLayout;
  head->setSpacing(theme::space::sm);
  head->addWidget(make_label(model_part_name(d, part), "title"), 1);
  head->addWidget(make_chip(user ? tr("Yours") : st == "ambiguous" ? tr("Undecided") : st.startsWith("inferred") ? tr("Estimated") : tr("Seen"),
                            tone));
  lay->addLayout(head);
  QString what = p ? part_status_words(st) : QString();
  if (user) {
    const auto mp = d.masks.parts.find(part);
    what = mp == d.masks.parts.end() ? tr("Created by an edit.")
           : mp->second.define     ? tr("Defined by the pixels you painted: the part is exactly what its masks allow.")
                                   : tr("Labels the model's pixels you painted.");
  }
  auto* words = make_label(what, "secondary");
  words->setWordWrap(true);
  lay->addWidget(words);
  Region3 r;
  if (p) {
    std::vector<Region3> rs;
    for (int c : p->candidates) rs.push_back(candidate_region(d, c));
    r = bounds(rs);
  }
  if (r.empty()) r = part_bounds(d, part);
  std::vector<std::pair<QString, QString>> rows;
  if (d.align && !r.empty()) {
    rows.push_back({tr("Position"), position_name(QRectF(QPointF(r.lo.x(), r.lo.z()), QPointF(r.hi.x(), r.hi.z())), r.lo.y(),
                                                  r.hi.y(), d.align->dims)});
    rows.push_back({tr("Size"), size_text(r)});
  }
  if (p) rows.push_back({tr("Pieces"), QString::number(p->candidates.size())});
  if (const auto mp = d.masks.parts.find(part); mp != d.masks.parts.end()) {
    QStringList px;
    for (const auto& [role, m] : mp->second.masks)
      if (m.count() > 0) px << tr("%1 %2 px").arg(view_title(role)).arg(m.count());
    rows.push_back({tr("Masks"), px.isEmpty() ? tr("none painted yet") : px.join(QStringLiteral(" · "))});
  }
  if (!pj.is_null() && !pj.value("has_geometry", true)) rows.push_back({tr("Geometry"), tr("none yet")});
  lay->addWidget(facts(rows));
  auto* actions = new QHBoxLayout;
  actions->setSpacing(theme::space::sm);
  auto* frame = make_button(tr("Frame"), "ghost", icon(Icon::Frame));
  connect(frame, &QPushButton::clicked, this, &Inspector::frameRequested);
  actions->addWidget(frame);
  actions->addStretch();
  lay->addLayout(actions);
  if (p && p->status == CandStatus::Ambiguous) {
    lay->addWidget(make_label(tr("Is this part real?"), "section"));
    auto* row = new QHBoxLayout;
    auto* keep = make_button(tr("Keep it"), "primary");
    auto* remove = make_button(tr("Remove it"));
    const std::vector<int> cands = p->candidates;
    connect(keep, &QPushButton::clicked, this, [this, cands] {
      std::vector<UserDecision> dec;
      for (int c : cands) dec.push_back({c, true});
      emit applyParts(dec);
    });
    connect(remove, &QPushButton::clicked, this, [this, cands] {
      std::vector<UserDecision> dec;
      for (int c : cands) dec.push_back({c, false});
      emit applyParts(dec);
    });
    row->addWidget(keep);
    row->addWidget(remove);
    row->addStretch();
    lay->addLayout(row);
  }
  // Editing (each button is one undoable step), two per row so the panel never clips.
  auto* edit = new Section(tr("Edit"));
  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(theme::space::sm);
  grid->setVerticalSpacing(theme::space::sm);
  std::vector<QPushButton*> buttons{mirror_button(emit_action)};
  std::vector<std::tuple<QString, Icon, QString, QString>> items{
      {tr("Duplicate"), Icon::Parts, "part_duplicate", tr("A copy beside it (Ctrl+D)")},
      {tr("Transform…"), Icon::PushPull, "part_transform", tr("Move, rotate or scale")},
      {tr("Split…"), Icon::Volume, "part_split", tr("Split by a plane")},
      {tr("Rename…"), Icon::Settings, "part_rename", tr("Give the part a name (F2)")},
      {tr("Delete"), Icon::Close, "part_delete", tr("Remove the part (Del)")}};
  if (user) {
    items.push_back({tr("Paint masks"), Icon::Brush, "part_paint_masks", tr("Parts tool: paint this part's pixels on a drawing")});
    items.push_back({tr("Fill other views"), Icon::DrawingOverlay, "part_sync_masks",
                     tr("Project the part into the drawings where it has no mask yet")});
  }
  for (const auto& [text, ic, key, tip] : items) {
    auto* b = make_button(text, nullptr, icon(ic));
    b->setToolTip(tip);
    const QString k = key;
    connect(b, &QPushButton::clicked, this, [this, k] { emit actionRequested(k); });
    buttons.push_back(b);
  }
  for (size_t i = 0; i < buttons.size(); ++i) {
    buttons[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    grid->addWidget(buttons[i], static_cast<int>(i / 2), static_cast<int>(i % 2));
  }
  edit->body()->addLayout(grid);
  lay->addWidget(edit);
  if (p) {
    auto* adv = advancedSection(tr("Technical details"));
    const std::vector<CandidateInfo> infos = candidate_infos(d, p->candidates);
    for (const CandidateInfo& ci : infos) {
      QString reason;
      if (d.candidates && ci.id < static_cast<int>(d.candidates->cands.size()))
        reason = qs(d.candidates->cands[static_cast<size_t>(ci.id)].reason);
      auto* l = make_label(tr("#%1 %2 · Top [%3, %4, %5, %6] · volume %7%8")
                               .arg(ci.id)
                               .arg(ci.status)
                               .arg(ci.footprint.left())
                               .arg(ci.footprint.top())
                               .arg(ci.footprint.right() + 1)
                               .arg(ci.footprint.bottom() + 1)
                               .arg(ci.volume, 0, 'f', 0)
                               .arg(reason.isEmpty() ? QString() : QStringLiteral(" · ") + reason),
                           "small");
      l->setWordWrap(true);
      adv->body()->addWidget(l);
    }
    lay->addWidget(adv);
  } else {
    auto* adv = advancedSection(tr("Technical details"));
    adv->body()->addWidget(facts({{tr("Part id"), QString::number(part)}}));
    lay->addWidget(adv);
  }
  return w;
}

QWidget* Inspector::buildFace(int t) {
  const Document& d = view_.doc;
  if (!d.solid || t < 0 || t >= static_cast<int>(d.solid->mesh.tris.size())) return nullptr;
  const Mesh& m = d.solid->mesh;
  const int region = t < static_cast<int>(m.tri_region.size()) ? m.tri_region[static_cast<size_t>(t)] : -1;
  double area = 0;
  for (size_t k = 0; k < m.tris.size(); ++k)
    if (static_cast<int>(k) == t || (region >= 0 && k < m.tri_region.size() && m.tri_region[k] == region))
      area += 0.5 * length(tri_normal_raw(m, k));
  const vec3d n = normalize(tri_normal_raw(m, static_cast<size_t>(t)));
  auto* w = new QWidget;
  auto* lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::md);
  lay->addWidget(navRow(tr("Overview")));
  lay->addWidget(make_label(tr("Face"), "title"));
  QString source = tr("Untextured — reconstruct with textures to see where its pixels come from");
  Tone tone = Tone::Neutral;
  if (d.texture && t < static_cast<int>(d.texture->faces.size())) {
    const FaceTexture& f = d.texture->faces[static_cast<size_t>(t)];
    const QString v = view_title(f.view);
    switch (f.mode) {
      case SurfaceMode::Seen: source = tr("Painted from the %1 drawing").arg(v); break;
      case SurfaceMode::OppositeProjection: source = tr("Hidden surface estimated — copied from the %1 drawing").arg(v); break;
      case SurfaceMode::EdgeExtend: source = tr("Hidden surface estimated — extended from nearby edges"); break;
      case SurfaceMode::FlatPalette: source = tr("Hidden surface estimated — flat palette colour"); break;
      case SurfaceMode::Manual: source = tr("Texture assigned by you (%1 drawing)").arg(v); break;
    }
    if (f.inferred) tone = Tone::Info;
  }
  auto* src = make_label(source, "secondary");
  src->setWordWrap(true);
  if (tone == Tone::Info) src->setProperty("tone", "info");
  lay->addWidget(src);
  lay->addWidget(facts({{tr("Direction"), facing(n)}, {tr("Area"), tr("%1 px²").arg(std::lround(area))}}));
  const int part = t < static_cast<int>(m.tri_part.size()) ? m.tri_part[static_cast<size_t>(t)] : -1;
  if (part >= 0) {
    QString words = part >= kFirstUserPart ? tr("Your part") : QString();
    Tone pt = Tone::Accent;
    if (d.correspondence)
      for (const Part3D& p : d.correspondence->parts)
        if (p.id == part) words = part_status_words(qs(to_string(p.status))), pt = part_status_tone(qs(to_string(p.status)));
    auto* row = make_row(icon(pt == Tone::Pass ? Icon::StatusPass : pt == Tone::Accent ? Icon::Parts : tone_icon(pt), IconStyle::Toned, pt),
                         model_part_name(d, part), words);
    connect(row, &QPushButton::clicked, this, [this, part] { emit partRequested(part); });
    lay->addWidget(row);
  }
  auto* actions = new QHBoxLayout;
  auto* frame = make_button(tr("Frame"), "ghost", icon(Icon::Frame));
  connect(frame, &QPushButton::clicked, this, &Inspector::frameRequested);
  actions->addWidget(frame);
  actions->addStretch();
  lay->addLayout(actions);
  // Push/pull this face (one undoable step); dragging it with the Push/Pull tool does the same.
  auto* pp = new Section(tr("Push / pull"));
  auto* prow = new QHBoxLayout;
  prow->setSpacing(theme::space::sm);
  auto* dist = new QDoubleSpinBox;
  dist->setRange(-1000, 1000);
  dist->setDecimals(1);
  dist->setValue(1);
  dist->setSuffix(tr(" px"));
  dist->setToolTip(tr("Positive pulls the face out, negative pushes it in"));
  auto* go = make_button(tr("Apply"), nullptr, icon(Icon::PushPull));
  go->setObjectName("facePushPull");
  connect(go, &QPushButton::clicked, this, [this, dist] { emit pushPullRequested(dist->value()); });
  prow->addWidget(dist);
  prow->addWidget(go);
  prow->addStretch();
  pp->body()->addLayout(prow);
  auto* pnote = make_label(tr("Pulls stop at the drawings' outline so the model keeps matching them."), "small");
  pnote->setWordWrap(true);
  pp->body()->addWidget(pnote);
  lay->addWidget(pp);
  auto* adv = advancedSection(tr("Technical details"));
  adv->body()->addWidget(facts({{tr("Triangle"), QString::number(t)},
                                {tr("Face id"), QString::number(region)},
                                {tr("Normal"), QStringLiteral("%1, %2, %3").arg(n.x, 0, 'f', 2).arg(n.y, 0, 'f', 2).arg(n.z, 0, 'f', 2)}}));
  lay->addWidget(adv);
  return w;
}

}  // namespace p3dgui
