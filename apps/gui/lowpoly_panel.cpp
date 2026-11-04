#include "lowpoly_panel.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include "pixel3d/engine/lowpoly_stage.hpp"
#include "qt_util.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;

namespace {

const lowpoly::PartType kTypes[] = {lowpoly::PartType::Head, lowpoly::PartType::Torso, lowpoly::PartType::Limb,
                                    lowpoly::PartType::Accessory, lowpoly::PartType::Cloth};

QString type_title(lowpoly::PartType t) {
  switch (t) {
    case lowpoly::PartType::Head: return QObject::tr("Head");
    case lowpoly::PartType::Torso: return QObject::tr("Torso");
    case lowpoly::PartType::Limb: return QObject::tr("Limb");
    case lowpoly::PartType::Accessory: return QObject::tr("Accessory");
    case lowpoly::PartType::Cloth: return QObject::tr("Cloth");
  }
  return {};
}

QHBoxLayout* labelled(const QString& label, QWidget* w) {
  auto* row = new QHBoxLayout;
  row->addWidget(make_label(label, "small"));
  row->addStretch();
  row->addWidget(w);
  return row;
}

}  // namespace

LowpolyPanel::LowpolyPanel(const SessionView& v, bool posed, QWidget* parent) : QWidget(parent) {
  const Document& d = v.doc;
  settings_ = d.settings.lowpoly;
  const lowpoly::Settings& b = settings_.build;
  const LowpolyBuild* built = current_lowpoly(d);
  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(theme::space::md);

  // ---- parts ----
  auto* parts_sec = new Section(tr("Low-Poly Studio · parts"));
  parts_sec->setObjectName("lowpolyParts");
  if (d.masks.parts.empty()) {
    auto* hint = make_label(tr("A low-poly character is built part by part. Let 3re suggest typed parts from the "
                               "Front drawing (you confirm them), or paint parts yourself with the Parts tool."),
                            "secondary");
    hint->setWordWrap(true);
    parts_sec->body()->addWidget(hint);
  }
  json report = json::object();
  if (built) report = lowpoly_report(*built);
  std::map<int, json> part_rows;
  for (const auto& p : report.value("parts", json::array())) part_rows[p.value("id", -1)] = p;
  parts_ = new QTableWidget(0, 4);
  parts_->setObjectName("lowpolyPartTable");
  parts_->setHorizontalHeaderLabels({tr("Part"), tr("Type"), tr("Tris"), tr("Depth")});
  parts_->verticalHeader()->hide();
  parts_->setSelectionMode(QAbstractItemView::NoSelection);
  parts_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  parts_->setFont(theme::font(theme::Type::Small));
  parts_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  parts_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  parts_->verticalHeader()->setDefaultSectionSize(26);
  for (const auto& [id, up] : d.masks.parts) {
    const int r = parts_->rowCount();
    parts_->insertRow(r);
    const auto n = d.part_names.find(id);
    auto* name = new QTableWidgetItem(n != d.part_names.end() ? qs(n->second) : tr("part %1").arg(id));
    parts_->setItem(r, 0, name);
    auto* type = new QComboBox;
    type->setObjectName(QStringLiteral("lowpolyType_%1").arg(id));
    type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    type->setMinimumContentsLength(8);
    const auto t = d.part_types.find(id);
    if (t == d.part_types.end()) type->addItem(tr("— choose —"), -1);
    for (lowpoly::PartType k : kTypes) type->addItem(type_title(k), static_cast<int>(k));
    if (t != d.part_types.end()) type->setCurrentIndex(type->findData(static_cast<int>(t->second)));
    connect(type, &QComboBox::activated, this, [this, type, id] {
      const int k = type->currentData().toInt();
      if (k >= 0) emit partTypeRequested(id, static_cast<lowpoly::PartType>(k));
    });
    parts_->setCellWidget(r, 1, type);
    const auto pr = part_rows.find(id);
    parts_->setItem(r, 2, new QTableWidgetItem(pr != part_rows.end() ? QString::number(pr->second.value("triangles", 0)) : "—"));
    // Where the part's depth comes from: a drawing (measured) or estimated from its type (INFERRED).
    if (pr != part_rows.end()) {
      const std::string src = pr->second.value("depth_source", std::string());
      auto* depth = new QTableWidgetItem(src.find("inflation") != std::string::npos || src.rfind("mirror", 0) == 0
                                             ? tr("INFERRED")
                                             : qs(src));
      if (depth->text() == tr("INFERRED")) depth->setForeground(theme::colors().review);
      parts_->setItem(r, 3, depth);
    }
    if (pr != part_rows.end())
      name->setToolTip(tr("depth: %1 · depth/width %2")
                           .arg(qs(pr->second.value("depth_source", std::string())))
                           .arg(pr->second.value("depth_ratio", 0.0), 0, 'f', 2));
  }
  parts_->setFixedHeight(parts_->horizontalHeader()->sizeHint().height() + 26 * std::min(parts_->rowCount(), 10) + 4);
  parts_->setVisible(parts_->rowCount() > 0);
  parts_sec->body()->addWidget(parts_);
  auto* srow = new QHBoxLayout;
  suggest_ = make_button(d.masks.parts.empty() ? tr("Suggest parts…") : tr("Suggest again…"),
                         d.masks.parts.empty() ? "primary" : "ghost");
  suggest_->setObjectName("lowpolySuggest");
  suggest_->setToolTip(tr("Typed parts proposed from the Front drawing (head, body, limbs, accessories). Nothing changes "
                          "until you accept the proposal."));
  connect(suggest_, &QPushButton::clicked, this, &LowpolyPanel::suggestRequested);
  srow->addWidget(suggest_);
  // Layered drawings (.aseprite, .psd): their layers as suggested parts.
  bool layered = false;
  for (const SourceView& sv : d.views)
    if (sv.original_bytes) {
      const ImageFormat f = sniff_image_format(*sv.original_bytes);
      layered |= f == ImageFormat::Aseprite || f == ImageFormat::Psd;
    }
  layers_ = make_button(tr("Parts from layers…"), d.masks.parts.empty() && layered ? "primary" : "ghost");
  layers_->setObjectName("lowpolyLayerParts");
  layers_->setToolTip(tr("Each visible layer of the drawing becomes a suggested part named after the layer. You choose "
                         "which ones to keep."));
  layers_->setVisible(layered);
  connect(layers_, &QPushButton::clicked, this, &LowpolyPanel::layerPartsRequested);
  srow->addWidget(layers_);
  srow->addStretch();
  parts_sec->body()->addLayout(srow);
  lay->addWidget(parts_sec);

  // ---- build ----
  auto* build_sec = new Section(tr("Low-Poly Studio · build"));
  build_sec->setObjectName("lowpolyBuild");
  budget_slider_ = new QSlider(Qt::Horizontal);
  budget_slider_->setRange(lowpoly::kMinBudget, 1500);
  budget_slider_->setSingleStep(50);
  budget_slider_->setPageStep(100);
  budget_ = new QSpinBox;
  budget_->setObjectName("lowpolyBudget");
  budget_->setRange(lowpoly::kMinBudget, lowpoly::kMaxBudget);
  budget_->setSingleStep(50);
  budget_->setSuffix(tr(" tris"));
  budget_->setValue(b.tri_budget);
  budget_slider_->setValue(std::min(b.tri_budget, 1500));
  connect(budget_slider_, &QSlider::valueChanged, budget_, [this](int x) {
    if (budget_->value() <= 1500) budget_->setValue(x);
  });
  connect(budget_, &QSpinBox::valueChanged, budget_slider_, [this](int x) { budget_slider_->setValue(std::min(x, 1500)); });
  build_sec->body()->addLayout(labelled(tr("Triangle budget"), budget_));
  build_sec->body()->addWidget(budget_slider_);
  auto* note = make_label(tr("A hard cap on the exported triangles; nothing is decimated. 500 is the minimum (warned)."), "small");
  note->setWordWrap(true);
  build_sec->body()->addWidget(note);
  loops_ = new QSpinBox;
  loops_->setObjectName("lowpolyJointLoops");
  loops_->setRange(1, 3);
  loops_->setValue(b.joint_loops);
  build_sec->body()->addLayout(labelled(tr("Joint loops"), loops_));
  style_ = new QComboBox;
  style_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  style_->setMinimumContentsLength(10);
  style_->setObjectName("lowpolyStyle");
  for (lowpoly::Style s : {lowpoly::Style::CelOutline, lowpoly::Style::Cel, lowpoly::Style::Outline, lowpoly::Style::Flat})
    style_->addItem(s == lowpoly::Style::CelOutline ? tr("Cel + outline")
                    : s == lowpoly::Style::Cel      ? tr("Cel")
                    : s == lowpoly::Style::Outline  ? tr("Outline")
                                                    : tr("Flat (unlit)"),
                    qs(lowpoly::to_string(s)));
  style_->setCurrentIndex(std::max(0, style_->findData(qs(lowpoly::to_string(b.style)))));
  build_sec->body()->addLayout(labelled(tr("Look"), style_));
  template_ = new QComboBox;
  template_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  template_->setMinimumContentsLength(10);
  template_->setObjectName("lowpolyTemplate");
  for (lowpoly::Template t : {lowpoly::Template::Auto, lowpoly::Template::CloakedCharacter, lowpoly::Template::Humanoid,
                              lowpoly::Template::GenericChain})
    template_->addItem(t == lowpoly::Template::Auto               ? tr("Automatic")
                       : t == lowpoly::Template::CloakedCharacter ? tr("Cloaked character")
                       : t == lowpoly::Template::Humanoid         ? tr("Humanoid")
                                                                  : tr("Generic chain"),
                       qs(lowpoly::to_string(t)));
  template_->setCurrentIndex(std::max(0, template_->findData(qs(lowpoly::to_string(b.templ)))));
  build_sec->body()->addLayout(labelled(tr("Body plan"), template_));
  baked_ = new QCheckBox(tr("Bake shading into the texture"));
  baked_->setToolTip(tr("For engines without shaders (e.g. Roblox): the cel ramp is baked with a fixed light and will "
                        "not follow the engine's lights"));
  baked_->setObjectName("lowpolyBakedCel");
  baked_->setChecked(b.baked_cel);
  build_sec->body()->addWidget(baked_);
  bool front_only = true;
  for (const SourceView& sv : d.views) front_only &= sv.role == ViewRole::Front;
  accept_depth_ = new QCheckBox(tr("Accept the estimated depth for export"));
  accept_depth_->setToolTip(tr("Front only: every depth is estimated. Accepting allows export and batch use; the status "
                               "stays NEEDS REVIEW and the report says you accepted it"));
  accept_depth_->setObjectName("lowpolyAcceptDepth");
  accept_depth_->setChecked(b.accept_inferred_depth);
  accept_depth_->setVisible(front_only);
  front_only_ = front_only;
  if (front_only && !d.views.empty()) {
    auto* fo = make_label(tr("Front only: every part's depth is INFERRED from its type (head, torso, limb, cloth, "
                             "accessory). The character stays NEEDS REVIEW; export needs your acceptance below."),
                          "small");
    fo->setObjectName("lowpolyFrontOnlyNote");
    fo->setWordWrap(true);
    fo->setProperty("tone", "review");
    build_sec->body()->addWidget(fo);
  }
  build_sec->body()->addWidget(accept_depth_);
  auto* actions = new QHBoxLayout;
  build_ = make_button(built ? tr("Rebuild") : tr("Build character"), built ? nullptr : "primary");
  build_->setObjectName("lowpolyBuildButton");
  build_->setEnabled(!d.masks.parts.empty());
  connect(build_, &QPushButton::clicked, this, [this] { emit buildRequested(update()); });
  actions->addWidget(build_);
  auto* pose = make_button(posed ? tr("Rest pose") : tr("Test pose"), "ghost");
  pose->setObjectName("lowpolyPose");
  pose->setCheckable(true);
  pose->setChecked(posed);
  pose->setEnabled(built != nullptr);
  pose->setToolTip(tr("Show the rig's test pose (skinned) to see how the joints bend"));
  connect(pose, &QPushButton::toggled, this, &LowpolyPanel::poseToggled);
  actions->addWidget(pose);
  actions->addStretch();
  auto* exp = make_button(tr("Export…"), built && built->validation.status == ValidationStatus::Pass ? "primary" : "ghost");
  exp->setObjectName("lowpolyExport");
  exp->setEnabled(built != nullptr && built->validation.status != ValidationStatus::Fail);
  connect(exp, &QPushButton::clicked, this, &LowpolyPanel::exportRequested);
  actions->addWidget(exp);
  build_sec->body()->addLayout(actions);

  // ---- result ----
  if (built) {
    const auto& lp = report;
    QStringList lines;
    lines << tr("%1 / %2 triangles · %3 · %4 bones")
                 .arg(lp.value("triangles", 0))
                 .arg(lp.value("budget", 0))
                 .arg(qs(lp.value("template", std::string())))
                 .arg(lp.value("bones", 0));
    const json ious = lp.value("silhouette_iou", json::object());  // keep alive while iterating
    for (const auto& [role, iou] : ious.items())
      lines << tr("%1 silhouette match: %2 %").arg(qs(role)).arg(iou.get<double>() * 100, 0, 'f', 1);
    if (lp.contains("surface"))
      for (const auto& [mode, share] : lp["surface"].items())
        if (mode.rfind("seen", 0) != 0) lines << tr("hidden surface %1: %2 %").arg(qs(mode)).arg(share.get<double>() * 100, 0, 'f', 1);
    auto* res = make_label(lines.join('\n'), "small");
    res->setObjectName("lowpolyResult");
    res->setWordWrap(true);
    res->setTextInteractionFlags(Qt::TextSelectableByMouse);
    build_sec->body()->addWidget(res);
    for (const auto& c : lp.value("checks", json::array()))
      if (!c.value("passed", true)) {
        auto* l = make_label((c.value("critical", false) ? tr("FAIL: %1") : tr("Check: %1")).arg(qs(c.value("text", std::string()))), "small");
        l->setWordWrap(true);
        l->setProperty("tone", c.value("critical", false) ? "fail" : "review");
        build_sec->body()->addWidget(l);
      }
    for (const auto& c : lp.value("conflicts", json::array()))
      if (c.value("blocking", false)) {
        auto* l = make_label(tr("Needs review: %1").arg(qs(c.value("text", std::string()))), "small");
        l->setWordWrap(true);
        l->setProperty("tone", "review");
        build_sec->body()->addWidget(l);
      }
  }
  lay->addWidget(build_sec);
}

LowpolyUpdate LowpolyPanel::update() const {
  LowpolyUpdate u;
  const lowpoly::Settings& b = settings_.build;
  if (budget_->value() != b.tri_budget) u.tri_budget = budget_->value();
  if (loops_->value() != b.joint_loops) u.joint_loops = loops_->value();
  if (auto s = lowpoly::style_from_string(utf8(style_->currentData().toString())); s && *s != b.style) u.style = *s;
  if (auto t = lowpoly::template_from_string(utf8(template_->currentData().toString())); t && *t != b.templ) u.templ = *t;
  if (baked_->isChecked() != b.baked_cel) u.baked_cel = baked_->isChecked();
  if (front_only_ && accept_depth_->isChecked() != b.accept_inferred_depth)
    u.accept_inferred_depth = accept_depth_->isChecked();
  return u;
}

}  // namespace p3dgui
