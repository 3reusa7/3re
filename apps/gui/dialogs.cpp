#include "dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "issues.hpp"
#include "qt_util.hpp"
#include "views_panel.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;

namespace {

QDialogButtonBox* ok_cancel(QDialog* d) {
  auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(b, &QDialogButtonBox::accepted, d, &QDialog::accept);
  QObject::connect(b, &QDialogButtonBox::rejected, d, &QDialog::reject);
  return b;
}

}  // namespace

// ---- new project ------------------------------------------------------------

NewProjectDialog::NewProjectDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("New project"));
  auto* form = new QFormLayout(this);
  name_ = new QLineEdit(QStringLiteral("untitled"));
  ppm_ = new QDoubleSpinBox;
  ppm_->setRange(0.001, 1000000);
  ppm_->setDecimals(3);
  ppm_->setValue(32);
  ppm_->setToolTip(tr("How many source pixels make one metre in the exported GLB"));
  profile_ = new QComboBox;
  profile_->addItem(tr("Hand-drawn art (tolerant)"), static_cast<int>(ToleranceProfile::Hand));
  profile_->addItem(tr("Synthetic / exact art (strict)"), static_cast<int>(ToleranceProfile::Strict));
  alpha_ = new QSpinBox;
  alpha_->setRange(1, 255);
  alpha_->setValue(128);
  alpha_->setToolTip(tr("Pixels with at least this alpha are opaque"));
  form->addRow(tr("Name"), name_);
  form->addRow(tr("Pixels per metre"), ppm_);
  form->addRow(tr("Tolerance"), profile_);
  form->addRow(tr("Alpha threshold"), alpha_);
  // Real-world images: what auto-alignment may do to drawings that are not clean pixel art.
  const NormalizeSettings def;
  normalize_ = new QCheckBox(tr("Clean up real-world images (backgrounds, anti-aliasing, size)"));
  normalize_->setChecked(def.enabled);
  normalize_->setToolTip(tr("Removes an opaque background, merges anti-aliasing and noise colours and shrinks large art. "
                            "Clean pixel art is never changed. Every change is listed in the log."));
  target_ = new QSpinBox;
  target_->setRange(16, 512);
  target_->setSuffix(tr(" px"));
  target_->setValue(def.target_size);
  target_->setToolTip(tr("Longest side of the model after shrinking large images"));
  bg_tol_ = new QSpinBox;
  bg_tol_->setRange(0, 255);
  bg_tol_->setValue(def.bg_tolerance);
  bg_tol_->setToolTip(tr("How far a colour may be from the background colour and still be removed. Lower it if parts of "
                         "the object disappear."));
  auto* section = new QLabel(tr("Real-world images"));
  section->setProperty("type", "section");
  form->addRow(section);
  form->addRow(normalize_);
  form->addRow(tr("Largest side"), target_);
  form->addRow(tr("Background tolerance"), bg_tol_);
  connect(normalize_, &QCheckBox::toggled, this, [this](bool on) {
    target_->setEnabled(on);
    bg_tol_->setEnabled(on);
  });
  // The model style: crisp is the pixel-exact model; the others are polished from it.
  auto* look = new QLabel(tr("Style"));
  look->setProperty("type", "section");
  form->addRow(look);
  style_ = new QComboBox;
  style_->setObjectName(QStringLiteral("styleCombo"));
  style_->addItem(tr("Crisp — pixel-exact blocks"), static_cast<int>(StylePreset::Crisp));
  style_->addItem(tr("Soft — rounded, smooth shading"), static_cast<int>(StylePreset::Soft));
  style_->addItem(tr("Low-poly — chamfered, flat shading"), static_cast<int>(StylePreset::Lowpoly));
  style_->addItem(tr("Clay — rounded, matte"), static_cast<int>(StylePreset::Clay));
  style_->setToolTip(tr("Soft, Low-poly and Clay round the model inside your drawings: every drawn pixel stays "
                        "covered. The styled model is built when you validate, and it is the one that is checked "
                        "and exported. Crisp is the model you edit."));
  form->addRow(tr("Model style"), style_);
  // What the project builds: a rigged low-poly character (the default: no Top drawing needed), or the
  // visual-hull model, which needs all three drawings and is chosen explicitly.
  mode_ = new QComboBox;
  mode_->setObjectName(QStringLiteral("projectMode"));
  mode_->addItem(tr("Low-Poly Studio — Front + Side, or Front only (no Top needed)"), true);
  mode_->addItem(tr("Visual Hull — needs Front + Side + Top"), false);
  mode_->setToolTip(tr("Low-Poly Studio builds a game-ready low-poly character part by part from the Front drawing "
                       "(and Side or Top when you have them), with a skeleton and a triangle budget. Visual Hull "
                       "carves an exact solid from Front, Side and Top and refuses to build without all three."));
  form->addRow(tr("Project type"), mode_);
  connect(mode_, &QComboBox::currentIndexChanged, this, [this] { style_->setEnabled(!mode_->currentData().toBool()); });
  lowpoly_ = default_project_settings().lowpoly;
  mode_->setCurrentIndex(mode_->findData(lowpoly_.enabled));
  style_->setEnabled(!lowpoly_.enabled);
  form->addRow(ok_cancel(this));
}

ProjectSettings default_project_settings() {
  ProjectSettings s;
  s.lowpoly.enabled = true;  // most artists have no Top drawing: Visual Hull is an explicit choice
  return s;
}

void NewProjectDialog::setSettings(const ProjectSettings& s) {
  name_->setText(qs(s.name));
  ppm_->setValue(s.pixels_per_meter);
  profile_->setCurrentIndex(profile_->findData(static_cast<int>(s.profile)));
  alpha_->setValue(s.alpha_threshold);
  normalize_->setChecked(s.normalize.enabled);
  target_->setValue(s.normalize.target_size);
  bg_tol_->setValue(s.normalize.bg_tolerance);
  axis_tolerance_ = s.normalize.axis_tolerance;
  style_settings_ = s.style;
  style_->setCurrentIndex(style_->findData(static_cast<int>(s.style.preset)));
  lowpoly_ = s.lowpoly;
  mode_->setCurrentIndex(mode_->findData(s.lowpoly.enabled));
  style_->setEnabled(!s.lowpoly.enabled);
}

ProjectSettings NewProjectDialog::settings() const {
  ProjectSettings s;
  s.name = utf8(name_->text().trimmed().isEmpty() ? QStringLiteral("untitled") : name_->text().trimmed());
  s.pixels_per_meter = ppm_->value();
  s.profile = static_cast<ToleranceProfile>(profile_->currentData().toInt());
  s.alpha_threshold = static_cast<uint8_t>(alpha_->value());
  s.normalize.enabled = normalize_->isChecked();
  s.normalize.target_size = target_->value();
  s.normalize.bg_tolerance = bg_tol_->value();
  s.normalize.axis_tolerance = axis_tolerance_;
  s.style = style_settings_;
  s.style.preset = static_cast<StylePreset>(style_->currentData().toInt());
  s.lowpoly = lowpoly_;
  s.lowpoly.enabled = mode_->currentData().toBool();
  return s;
}

// ---- recovery ---------------------------------------------------------------------

std::optional<RecoverableSession> ask_recovery(QWidget* parent, const std::vector<RecoverableSession>& sessions,
                                               bool* discard_all) {
  *discard_all = false;
  if (sessions.empty()) return std::nullopt;
  // Newest first.
  std::vector<RecoverableSession> s = sessions;
  std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.saved_at > b.saved_at; });
  QString list;
  for (const auto& r : s)
    list += QStringLiteral("• %1 — saved %2\n")
                .arg(r.original_project.empty() ? QObject::tr("unsaved project") : qs(r.original_project), qs(r.saved_at));
  QMessageBox box(parent);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(QObject::tr("Recover work"));
  box.setText(QObject::tr("3re did not close normally. Unsaved work was autosaved:"));
  box.setInformativeText(list);
  QPushButton* recover = box.addButton(QObject::tr("Recover newest"), QMessageBox::AcceptRole);
  QPushButton* discard = box.addButton(QObject::tr("Discard"), QMessageBox::DestructiveRole);
  box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
  box.exec();
  if (box.clickedButton() == recover) return s.front();
  if (box.clickedButton() == discard) *discard_all = true;
  return std::nullopt;
}

// ---- single view ------------------------------------------------------------------

SingleViewDialog::SingleViewDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("New single-view project"));
  auto* form = new QFormLayout(this);
  auto* note = new QLabel(tr("One drawing gives the silhouette only. The depth you choose here is applied to the whole "
                             "shape and every hidden side is marked INFERRED; the model stays NEEDS_REVIEW until you "
                             "confirm a depth. It is never as accurate as Front + Side + Top."));
  note->setWordWrap(true);
  note->setStyleSheet("color: #d9a441;");
  form->addRow(note);
  path_ = new QLineEdit;
  auto* browse = new QPushButton(tr("…"));
  auto* row = new QHBoxLayout;
  row->addWidget(path_, 1);
  row->addWidget(browse);
  form->addRow(tr("Drawing"), row);
  role_ = new QComboBox;
  for (ViewRole r : kAllViewRoles) role_->addItem(view_title(r), static_cast<int>(r));
  form->addRow(tr("It shows the"), role_);
  preset_ = new QComboBox;
  preset_->addItem(tr("Choose later (NEEDS_REVIEW)"), QString());
  preset_->addItem(tr("Flat (1 px)"), "flat");
  preset_->addItem(tr("Thin (1/4 of the smaller side)"), "thin");
  preset_->addItem(tr("Medium (1/2 of the smaller side)"), "medium");
  preset_->addItem(tr("Thick (= the smaller side)"), "thick");
  preset_->addItem(tr("Custom depth"), "custom");
  form->addRow(tr("Depth"), preset_);
  depth_ = new QDoubleSpinBox;
  depth_->setRange(0.25, 4096);
  depth_->setValue(4);
  depth_->setEnabled(false);
  form->addRow(tr("Custom depth (px)"), depth_);
  name_ = new QLineEdit(QStringLiteral("untitled"));
  form->addRow(tr("Project name"), name_);
  form->addRow(ok_cancel(this));
  connect(preset_, &QComboBox::currentIndexChanged, this,
          [this] { depth_->setEnabled(preset_->currentData().toString() == "custom"); });
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString f = QFileDialog::getOpenFileName(this, tr("Drawing"), {}, source_image_filter());
    if (!f.isEmpty()) setPath(f);
  });
}

void SingleViewDialog::setPath(const QString& path) {
  path_->setText(path);
  if (const auto r = role_from_filename(path)) role_->setCurrentIndex(role_->findData(static_cast<int>(*r)));
  if (name_->text() == "untitled") name_->setText(QFileInfo(path).completeBaseName());
}

QString SingleViewDialog::path() const { return path_->text(); }

ViewRole SingleViewDialog::role() const { return static_cast<ViewRole>(role_->currentData().toInt()); }

SingleViewSettings SingleViewDialog::settings() const {
  SingleViewSettings s;
  s.enabled = true;
  s.role = role();
  const QString p = preset_->currentData().toString();
  s.preset = p.isEmpty() ? "medium" : utf8(p);
  s.user_depth = !p.isEmpty();
  if (p == "custom") s.depth = depth_->value();
  return s;
}

ProjectSettings SingleViewDialog::project() const {
  ProjectSettings ps;
  ps.name = utf8(name_->text().trimmed().isEmpty() ? QStringLiteral("untitled") : name_->text().trimmed());
  return ps;
}

// ---- transform ----------------------------------------------------------------------

TransformDialog::TransformDialog(int part, bool copy, QWidget* parent) : QDialog(parent), part_(part), copy_(copy) {
  setWindowTitle(copy ? tr("Duplicate part %1").arg(part) : tr("Transform part %1").arg(part));
  auto* form = new QFormLayout(this);
  auto spin = [](double lo, double hi, double v) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setDecimals(2);
    s->setValue(v);
    return s;
  };
  tx_ = spin(-4096, 4096, copy ? 2 : 0);
  ty_ = spin(-4096, 4096, 0);
  tz_ = spin(-4096, 4096, 0);
  auto* move = new QHBoxLayout;
  for (auto* s : {tx_, ty_, tz_}) move->addWidget(s);
  form->addRow(tr("Move x, y, z (px)"), move);
  axis_ = new QComboBox;
  axis_->addItem("X", 0);
  axis_->addItem("Y", 1);
  axis_->addItem("Z", 2);
  axis_->setCurrentIndex(1);
  deg_ = spin(-360, 360, 0);
  auto* rot = new QHBoxLayout;
  rot->addWidget(axis_);
  rot->addWidget(deg_);
  form->addRow(tr("Rotate about (degrees)"), rot);
  scale_ = spin(0.01, 100, 1);
  form->addRow(tr("Scale"), scale_);
  snap_ = new QCheckBox(tr("Snap the move to whole pixels"));
  snap_->setChecked(true);
  form->addRow(snap_);
  form->addRow(ok_cancel(this));
}

TransformRequest TransformDialog::request() const {
  TransformRequest r;
  r.part = part_;
  r.copy = copy_;
  r.translate = {tx_->value(), ty_->value(), tz_->value()};
  r.rotate_axis = axis_->currentData().toInt();
  r.rotate_deg = deg_->value();
  r.scale = scale_->value();
  r.snap = snap_->isChecked();
  return r;
}

// ---- plane -----------------------------------------------------------------------------

PlaneDialog::PlaneDialog(Kind kind, Dims dims, QWidget* parent) : QDialog(parent), dims_(dims) {
  setWindowTitle(kind == Kind::Split ? tr("Split part by a plane") : tr("Cut with a plane"));
  auto* form = new QFormLayout(this);
  axis_ = new QComboBox;
  axis_->addItem(tr("X (left - right)"), 0);
  axis_->addItem(tr("Y (bottom - top)"), 1);
  axis_->addItem(tr("Z (back - front)"), 2);
  at_ = new QDoubleSpinBox;
  at_->setDecimals(2);
  form->addRow(tr("Plane normal"), axis_);
  form->addRow(tr("At (px)"), at_);
  if (kind == Kind::Cut) {
    keep_ = new QComboBox;
    keep_->addItem(tr("Keep the lower side (below the plane)"), -1);
    keep_->addItem(tr("Keep the upper side (above the plane)"), 1);
    form->addRow(tr("Keep"), keep_);
  }
  auto update_range = [this] {
    const int extent[3] = {dims_.W, dims_.H, dims_.D};
    const int e = extent[axis_->currentData().toInt()];
    at_->setRange(0, std::max(1, e));
    at_->setValue(e / 2.0);
  };
  connect(axis_, &QComboBox::currentIndexChanged, this, update_range);
  update_range();
  form->addRow(ok_cancel(this));
}

int PlaneDialog::axis() const { return axis_->currentData().toInt(); }
double PlaneDialog::at() const { return at_->value(); }
int PlaneDialog::keep() const { return keep_ ? keep_->currentData().toInt() : -1; }

// ---- volume ----------------------------------------------------------------------------

VolumeDialog::VolumeDialog(bool add, double extent, QWidget* parent) : QDialog(parent) {
  setWindowTitle(add ? tr("Add volume") : tr("Erase volume"));
  auto* form = new QFormLayout(this);
  auto* note = new QLabel(tr("Depth is measured from this view's side of the model's box (0 = the surface facing you)."));
  note->setWordWrap(true);
  form->addRow(note);
  from_ = new QDoubleSpinBox;
  to_ = new QDoubleSpinBox;
  for (auto* s : {from_, to_}) {
    s->setRange(-4096, 4096);
    s->setDecimals(2);
  }
  from_->setValue(add ? 0 : -1);
  to_->setValue(add ? 1 : extent + 1);
  form->addRow(tr("From depth (px)"), from_);
  form->addRow(tr("To depth (px)"), to_);
  form->addRow(ok_cancel(this));
}

double VolumeDialog::from() const { return from_->value(); }
double VolumeDialog::to() const { return to_->value(); }

// ---- export ------------------------------------------------------------------------------

ExportDialog::ExportDialog(const QString& suggestion, Dims dims, double pixels_per_meter, QWidget* parent)
    : QDialog(parent), dims_(dims), ppm_(pixels_per_meter) {
  setWindowTitle(tr("Export model"));
  setMinimumWidth(460);
  auto* form = new QFormLayout(this);
  path_ = new QLineEdit(QDir::toNativeSeparators(suggestion));
  auto* browse = new QPushButton(tr("…"));
  auto* row = new QHBoxLayout;
  row->addWidget(path_, 1);
  row->addWidget(browse);
  form->addRow(tr("File"), row);
  preset_ = new QComboBox;
  preset_->addItem(tr("Generic glTF"), static_cast<int>(EnginePreset::Generic));
  preset_->addItem(tr("Godot 4"), static_cast<int>(EnginePreset::Godot));
  preset_->addItem(tr("Unity"), static_cast<int>(EnginePreset::Unity));
  preset_->addItem(tr("Roblox"), static_cast<int>(EnginePreset::Roblox));
  form->addRow(tr("For"), preset_);
  format_ = new QComboBox;
  format_->addItem(tr("GLB — one file"), static_cast<int>(ExportFormat::Glb));
  format_->addItem(tr("glTF — .gltf + .bin + .png"), static_cast<int>(ExportFormat::Gltf));
  format_->addItem(tr("OBJ — .obj + .mtl + .png"), static_cast<int>(ExportFormat::Obj));
  form->addRow(tr("Format"), format_);
  units_ = new QComboBox;
  units_->addItem(tr("Metres"), static_cast<int>(ExportUnits::Metres));
  units_->addItem(tr("Centimetres"), static_cast<int>(ExportUnits::Centimetres));
  units_->addItem(tr("Studs (Roblox, 0.28 m)"), static_cast<int>(ExportUnits::Studs));
  units_->addItem(tr("Pixels (1 unit = 1 drawing pixel)"), static_cast<int>(ExportUnits::Pixels));
  form->addRow(tr("Units"), units_);
  front_ = new QComboBox;
  front_->addItem(tr("Front faces +Z (glTF, Godot, Unity)"), false);
  front_->addItem(tr("Front faces −Z (Roblox)"), true);
  form->addRow(tr("Facing"), front_);
  unlit_ = new QCheckBox(tr("Unlit material — exact pixel colours, no shading (glTF)"));
  form->addRow(unlit_);
  size_ = new QLabel;
  form->addRow(tr("Size"), size_);
  note_ = new QLabel;
  note_->setWordWrap(true);
  note_->setProperty("type", "small");
  form->addRow(note_);
  QDialogButtonBox* buttons = ok_cancel(this);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));
  form->addRow(buttons);
  connect(preset_, &QComboBox::currentIndexChanged, this, &ExportDialog::presetChanged);
  for (QComboBox* c : {format_, units_, front_}) connect(c, &QComboBox::currentIndexChanged, this, &ExportDialog::update);
  connect(unlit_, &QCheckBox::toggled, this, &ExportDialog::update);
  connect(path_, &QLineEdit::textChanged, this, [this](const QString& p) {
    // The extension picks the format (and the other way round).
    const QString ext = QFileInfo(p).suffix().toLower();
    if (const auto f = export_format_from_string(utf8(ext))) {
      const int i = format_->findData(static_cast<int>(*f));
      if (i >= 0 && i != format_->currentIndex()) format_->setCurrentIndex(i);
    }
  });
  connect(format_, &QComboBox::currentIndexChanged, this, [this] {
    if (updating_) return;
    const auto f = static_cast<ExportFormat>(format_->currentData().toInt());
    QString p = path_->text();
    if (p.isEmpty()) return;
    const QFileInfo fi(p);
    const QString want = qs(extension(f));
    if (("." + fi.suffix().toLower()) != want)
      path_->setText(QDir::toNativeSeparators(fi.path() + "/" + fi.completeBaseName() + want));
  });
  connect(browse, &QPushButton::clicked, this, [this] {
    const auto f = static_cast<ExportFormat>(format_->currentData().toInt());
    const QString filter = f == ExportFormat::Glb ? tr("glTF binary (*.glb)") : f == ExportFormat::Gltf ? tr("glTF (*.gltf)") : tr("Wavefront OBJ (*.obj)");
    const QString p = QFileDialog::getSaveFileName(this, tr("Export model"), path_->text(), filter);
    if (!p.isEmpty()) setPath(p);
  });
  presetChanged();
}

void ExportDialog::setPath(const QString& path) { path_->setText(QDir::toNativeSeparators(path)); }

void ExportDialog::setPreset(EnginePreset p) { preset_->setCurrentIndex(preset_->findData(static_cast<int>(p))); }

void ExportDialog::presetChanged() {
  const auto p = static_cast<EnginePreset>(preset_->currentData().toInt());
  const PresetDefaults d = preset_defaults(p);
  updating_ = true;
  units_->setCurrentIndex(units_->findData(static_cast<int>(d.units)));
  front_->setCurrentIndex(d.front_negative_z ? 1 : 0);
  updating_ = false;
  format_->setCurrentIndex(format_->findData(static_cast<int>(d.format)));
  note_->setText(qs(d.note));
  update();
}

void ExportDialog::update() {
  const auto f = static_cast<ExportFormat>(format_->currentData().toInt());
  unlit_->setEnabled(f != ExportFormat::Obj);
  const auto u = static_cast<ExportUnits>(units_->currentData().toInt());
  double scale = 1.0 / std::max(ppm_, 1e-9);
  QString unit = tr("m");
  switch (u) {
    case ExportUnits::Metres: break;
    case ExportUnits::Centimetres: scale *= 100, unit = tr("cm"); break;
    case ExportUnits::Studs: scale /= kMetresPerStud, unit = tr("studs"); break;
    case ExportUnits::Pixels: scale = 1, unit = tr("px"); break;
  }
  size_->setText(tr("%1 × %2 × %3 %4 (W × H × D)")
                     .arg(dims_.W * scale, 0, 'g', 4)
                     .arg(dims_.H * scale, 0, 'g', 4)
                     .arg(dims_.D * scale, 0, 'g', 4)
                     .arg(unit));
}

ModelExportRequest ExportDialog::request() const {
  ModelExportRequest r;
  r.path = to_path(QDir::fromNativeSeparators(path_->text().trimmed()));
  r.format = static_cast<ExportFormat>(format_->currentData().toInt());
  r.preset = static_cast<EnginePreset>(preset_->currentData().toInt());
  r.units = static_cast<ExportUnits>(units_->currentData().toInt());
  r.front_negative_z = front_->currentData().toBool();
  r.unlit = unlit_->isEnabled() && unlit_->isChecked();
  return r;
}

}  // namespace p3dgui
