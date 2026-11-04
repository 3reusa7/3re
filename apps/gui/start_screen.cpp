#include "start_screen.hpp"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

#include "icons.hpp"
#include "issues.hpp"
#include "qt_util.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;

namespace {

QStringList png_files(const QMimeData* md) {
  QStringList out;
  for (const QUrl& u : md->urls())
    if (is_source_image(u.toLocalFile())) out << u.toLocalFile();
  return out;
}

constexpr ViewRole kSlots[] = {ViewRole::Front, ViewRole::Side, ViewRole::Top};

Icon slot_icon(ViewRole r) {
  return r == ViewRole::Front ? Icon::ViewFront : r == ViewRole::Side ? Icon::ViewSide : Icon::ViewTop;
}

}  // namespace

// ---- slot ------------------------------------------------------------------------

StartSlot::StartSlot(ViewRole role, QWidget* parent) : QFrame(parent) {
  info_.role = role;
  setAcceptDrops(true);
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  setFixedHeight(220);
  setToolTip(tr("Drop the %1 drawing here, or click to choose it").arg(view_title(role)));
}

void StartSlot::setInfo(const DrawingInfo& info) {
  info_ = info;
  update();
}

void StartSlot::setCurrent(bool on) {
  current_ = on;
  update();
}

void StartSlot::paintEvent(QPaintEvent*) {
  const theme::Colors& c = theme::colors();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF box = QRectF(rect()).adjusted(1, 1, -1, -1);
  QPainterPath path;
  path.addRoundedRect(box, 14, 14);
  const bool lit = drag_over_ || (current_ && !info_.assigned);
  QColor bg = info_.assigned ? c.raised : drag_over_ ? theme::with_alpha(c.accent, 0.14) : c.panel;
  if (hover_ && !drag_over_) bg = c.hover;
  p.fillPath(path, bg);
  QPen pen(lit ? c.accent : info_.assigned ? c.border : c.border_strong, lit ? 1.5 : 1.0);
  if (!info_.assigned) pen.setStyle(Qt::DashLine);
  p.setPen(pen);
  p.drawPath(path);

  const QRectF art(16, 16, width() - 32, 116);
  if (info_.assigned) {
    paint_pixel_image(p, info_.image, art, 16);
  } else {
    const int s = 44;
    icon(slot_icon(info_.role), lit ? IconStyle::Toned : IconStyle::Default, theme::Tone::Accent)
        .paint(&p, QRect(static_cast<int>(art.center().x() - s / 2), static_cast<int>(art.center().y() - s / 2), s, s),
               Qt::AlignCenter, lit ? QIcon::Normal : QIcon::Disabled);
  }
  p.setPen(c.text);
  p.setFont(theme::font(theme::Type::Title));
  p.drawText(QRectF(8, 138, width() - 16, 24), Qt::AlignCenter, view_title(info_.role));
  p.setFont(theme::font(theme::Type::Small));
  p.setPen(c.text_muted);
  p.drawText(QRectF(8, 162, width() - 16, 18), Qt::AlignCenter, view_orientation(info_.role));
  const QRectF last(8, 186, width() - 16, 20);
  if (info_.assigned) {
    const QString t = tr("%1 × %2 px").arg(info_.size.width()).arg(info_.size.height());
    const int tw = QFontMetrics(theme::font(theme::Type::Small)).horizontalAdvance(t);
    const QRectF ir(last.center().x() - (tw + 20) / 2.0, last.y() + 1, 16, 16);
    icon(Icon::StatusPass, IconStyle::Toned, theme::Tone::Pass).paint(&p, ir.toRect());
    p.setPen(c.text_secondary);
    p.drawText(QRectF(ir.right() + 4, last.y(), tw + 4, last.height()), Qt::AlignLeft | Qt::AlignVCenter, t);
  } else {
    p.setPen(lit ? c.accent_hover : c.text_muted);
    p.drawText(last, Qt::AlignCenter, info_.optional ? tr("Optional — drop an image or click") : tr("Drop an image or click"));
  }
}

void StartSlot::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) emit browse(info_.role);
}

void StartSlot::dragEnterEvent(QDragEnterEvent* e) {
  if (png_files(e->mimeData()).isEmpty()) return;
  e->acceptProposedAction();
  drag_over_ = true;
  update();
}

void StartSlot::dragLeaveEvent(QDragLeaveEvent*) {
  drag_over_ = false;
  update();
}

void StartSlot::dropEvent(QDropEvent* e) {
  drag_over_ = false;
  update();
  const QStringList files = png_files(e->mimeData());
  if (files.isEmpty()) return;
  e->acceptProposedAction();
  emit filesDropped(files, info_.role);
}

void StartSlot::enterEvent(QEnterEvent*) {
  hover_ = true;
  update();
}

void StartSlot::leaveEvent(QEvent*) {
  hover_ = false;
  update();
}

// ---- screen ------------------------------------------------------------------------

StartScreen::StartScreen(QWidget* parent) : QWidget(parent) {
  setAcceptDrops(true);
  setProperty("surface", "app");
  setAttribute(Qt::WA_StyledBackground, true);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(theme::space::xxl, theme::space::xxl, theme::space::xxl, theme::space::xxl);
  outer->addStretch(3);
  auto* column = new QWidget;
  column->setMaximumWidth(760);
  auto* col = new QVBoxLayout(column);
  col->setContentsMargins(0, 0, 0, 0);
  col->setSpacing(theme::space::lg);

  auto* brand = new QHBoxLayout;
  brand->setSpacing(theme::space::sm);
  // The symbol on its own (128 px source), scaled smoothly to the screen's pixel ratio.
  auto* logo = new QLabel;
  logo->setPixmap(QPixmap(QStringLiteral(":/3re-mark.png")));
  logo->setScaledContents(true);
  logo->setFixedSize(32, 32);
  brand->addStretch();
  brand->addWidget(logo);
  brand->addWidget(make_label(QStringLiteral("3re"), "section"));
  brand->addStretch();
  col->addLayout(brand);
  auto* title = make_label(tr("Create a 3D model from images"), "display");
  title->setAlignment(Qt::AlignCenter);
  title->setWordWrap(true);
  col->addWidget(title);
  lead_ = make_label(QString(), "lead");
  lead_->setAlignment(Qt::AlignCenter);
  lead_->setWordWrap(true);
  col->addWidget(lead_);
  auto* image3d = make_button(tr("Have a photo or illustration instead? Image to 3D (AI)…"), "ghost", icon(Icon::ImageTo3D));
  image3d->setObjectName("startImage3D");
  image3d->setMinimumHeight(38);
  col->addWidget(image3d);
  connect(image3d, &QPushButton::clicked, this, &StartScreen::image3dRequested);
  // The project type, always in sight, with the switch to the other one (the drawings are kept).
  auto* mode_row = new QHBoxLayout;
  mode_row->setSpacing(theme::space::sm);
  mode_row->addStretch();
  mode_ = make_label(QString(), "secondary");
  mode_->setObjectName(QStringLiteral("startProjectType"));
  mode_->setTextFormat(Qt::RichText);
  mode_switch_ = make_button(QString(), "link");
  mode_switch_->setObjectName(QStringLiteral("startSwitchProjectType"));
  mode_row->addWidget(mode_);
  mode_row->addWidget(mode_switch_);
  mode_row->addStretch();
  col->addLayout(mode_row);
  connect(mode_switch_, &QPushButton::clicked, this, [this] { emit projectTypeRequested(!lowpoly_); });
  col->addSpacing(theme::space::sm);

  auto* row = new QHBoxLayout;
  row->setSpacing(theme::space::lg);
  for (size_t i = 0; i < 3; ++i) {
    auto* s = new StartSlot(kSlots[i]);
    slots_[i] = s;
    row->addWidget(s, 1);
    connect(s, &StartSlot::browse, this, &StartScreen::browseRequested);
    connect(s, &StartSlot::filesDropped, this, [this](const QStringList& files, ViewRole r) {
      emit filesDropped(files, files.size() == 1 ? std::optional<ViewRole>(r) : std::nullopt);
    });
  }
  col->addLayout(row);

  auto* steps = new QHBoxLayout;
  steps->setSpacing(theme::space::md);
  steps->addStretch();
  for (size_t i = 0; i < 4; ++i) {
    steps_[i] = new QLabel;
    steps_[i]->setTextFormat(Qt::RichText);
    steps->addWidget(steps_[i]);
    if (i < 3) steps->addWidget(make_label(QStringLiteral("—"), "muted"));
  }
  steps->addStretch();
  col->addLayout(steps);

  missing_ = new MissingViewsNotice;
  col->addWidget(missing_);
  connect(missing_, &MissingViewsNotice::switchRequested, this, [this] { emit projectTypeRequested(true); });
  connect(missing_, &MissingViewsNotice::addRequested, this, &StartScreen::browseRequested);

  auto* actions = new QHBoxLayout;
  actions->setSpacing(theme::space::sm);
  actions->addStretch();
  reconstruct_ = make_button(tr("Reconstruct"), "primary", icon(Icon::Reconstruct, IconStyle::OnAccent));
  reconstruct_->setMinimumWidth(170);
  reconstruct_->setMinimumHeight(38);
  reconstruct_->setIconSize(QSize(18, 18));
  auto* import = make_button(tr("Import views…"), nullptr, icon(Icon::Import));
  import->setMinimumHeight(38);
  auto* open = make_button(tr("Open project…"), "ghost", icon(Icon::Open));
  open->setMinimumHeight(38);
  actions->addWidget(reconstruct_);
  actions->addWidget(import);
  actions->addWidget(open);
  actions->addStretch();
  col->addLayout(actions);
  connect(reconstruct_, &QPushButton::clicked, this, &StartScreen::reconstructRequested);
  connect(import, &QPushButton::clicked, this, &StartScreen::importRequested);
  connect(open, &QPushButton::clicked, this, &StartScreen::openRequested);

  workspace_ = make_button(tr("Inspect the drawings first ›"), "link");
  auto* single = make_button(tr("Only one drawing? Start from a single view ›"), "link");
  single->setObjectName("singleViewLink");
  single->setToolTip(tr("Extrude one PNG to a depth you choose; everything it does not show is estimated"));
  auto* wrow = new QHBoxLayout;
  wrow->addStretch();
  wrow->addWidget(workspace_);
  wrow->addWidget(single);
  wrow->addStretch();
  col->addLayout(wrow);
  connect(workspace_, &QPushButton::clicked, this, &StartScreen::workspaceRequested);
  connect(single, &QPushButton::clicked, this, &StartScreen::singleViewRequested);

  auto* tip = make_label(tr("From Aseprite: File ▸ Export As… a PNG for each view. Files named front, side (or left) "
                            "and top are sorted automatically; Back, Right and Bottom views are optional."),
                         "small");
  tip->setAlignment(Qt::AlignCenter);
  tip->setWordWrap(true);
  col->addWidget(tip);
  recent_ = new QLabel;
  recent_->setProperty("type", "small");
  recent_->setAlignment(Qt::AlignCenter);
  recent_->setTextFormat(Qt::RichText);
  recent_->setWordWrap(true);
  connect(recent_, &QLabel::linkActivated, this, &StartScreen::recentRequested);
  col->addWidget(recent_);

  auto* center = new QHBoxLayout;
  center->addStretch();
  center->addWidget(column, 100);
  center->addStretch();
  outer->addLayout(center);
  outer->addStretch(4);
}

void StartScreen::update_from(const SessionView& v, bool busy) {
  const theme::Colors& c = theme::colors();
  lowpoly_ = v.doc.settings.lowpoly.enabled;
  lead_->setText(lowpoly_ ? tr("Drop your Front drawing, and the Side if you have one — no Top needed. 3re builds a "
                               "game-ready low-poly character, checks it, and exports a GLB.")
                          : tr("Drop your Front, Side and Top drawings. 3re builds a solid model, checks it against "
                               "every drawing, and exports a GLB."));
  mode_->setText(tr("Project type: <b>%1</b> · %2").arg(project_type_name(lowpoly_), project_type_needs(lowpoly_)));
  mode_switch_->setText(lowpoly_ ? tr("Have Front, Side and Top? Use Visual Hull instead ›")
                                 : tr("No Top drawing? Switch to Low-Poly Studio ›"));
  mode_switch_->setEnabled(!busy);
  missing_->update_from(v.doc);
  int filled = 0;
  int current = -1;
  for (size_t i = 0; i < 3; ++i) {
    const DrawingInfo info = drawing_info(v, kSlots[i]);
    slots_[i]->setInfo(info);
    filled += info.assigned;
    if (!info.assigned && current < 0 && !info.optional) current = static_cast<int>(i);
  }
  for (size_t i = 0; i < 3; ++i) slots_[i]->setCurrent(static_cast<int>(i) == current);
  const bool ready = lowpoly_ ? drawing_info(v, ViewRole::Front).assigned : filled == 3;
  const QString names[] = {view_title(ViewRole::Front),
                           lowpoly_ ? tr("Side (optional)") : view_title(ViewRole::Side),
                           lowpoly_ ? tr("Top (optional)") : view_title(ViewRole::Top),
                           lowpoly_ ? tr("Build character") : tr("Reconstruct")};
  for (size_t i = 0; i < 4; ++i) {
    const bool done = i < 3 ? drawing_info(v, kSlots[i]).assigned
                            : v.doc.solid != nullptr || (lowpoly_ && v.status.status != ValidationStatus::NotValidated);
    const bool now = i < 3 ? static_cast<int>(i) == current : ready;
    QString html;
    if (done) html = QStringLiteral("<span style='color:%1'>✓</span>&nbsp;<span style='color:%2'>%3</span>")
                         .arg(c.pass.name(), c.text_secondary.name(), names[i]);
    else if (now) html = QStringLiteral("<b style='color:%1'>%2&nbsp;&nbsp;%3</b>").arg(c.accent_hover.name()).arg(i + 1).arg(names[i]);
    else html = QStringLiteral("<span style='color:%1'>%2&nbsp;&nbsp;%3</span>").arg(c.text_muted.name()).arg(i + 1).arg(names[i]);
    steps_[i]->setText(html);
  }
  if (v.doc.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: Front is enough (Side and Top measure depth when present).
    const bool front = drawing_info(v, kSlots[0]).assigned;
    reconstruct_->setText(tr("Build character"));
    reconstruct_->setEnabled(front && !busy);
    reconstruct_->setToolTip(front ? tr("Suggest typed parts from the Front drawing, then build the low-poly character")
                                   : tr("Add the Front drawing first (Side is optional)"));
  } else {
    reconstruct_->setText(tr("Reconstruct"));
    reconstruct_->setEnabled(filled == 3 && !busy);
    reconstruct_->setToolTip(filled == 3 ? tr("Build the model, then check it against the drawings (F5)")
                                         : tr("Add Front, Side and Top first"));
  }
  workspace_->setVisible(!v.doc.views.empty());
}

void StartScreen::setRecent(const QStringList& paths) {
  QStringList links;
  for (const QString& p : paths.mid(0, 4))
    links << QStringLiteral("<a style='color:%1; text-decoration:none' href=\"%2\">%3</a>")
                 .arg(theme::colors().accent_hover.name(), p.toHtmlEscaped(), QFileInfo(p).completeBaseName().toHtmlEscaped());
  recent_->setText(links.isEmpty() ? QString() : tr("Recent:  %1").arg(links.join(QStringLiteral("  ·  "))));
  recent_->setVisible(!links.isEmpty());
}

void StartScreen::dragEnterEvent(QDragEnterEvent* e) {
  if (!png_files(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

void StartScreen::dropEvent(QDropEvent* e) {
  const QStringList files = png_files(e->mimeData());
  if (files.isEmpty()) return;
  e->acceptProposedAction();
  emit filesDropped(files, std::nullopt);
}

}  // namespace p3dgui
