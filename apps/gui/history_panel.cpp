#include "history_panel.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "issues.hpp"
#include "pixel3d/engine/inspect.hpp"
#include "qt_util.hpp"
#include "theme.hpp"
#include "widgets.hpp"

namespace p3dgui {

using namespace pixel3d;
using nlohmann::json;

namespace {

QString step_title(const std::string& op) {
  static const std::map<std::string, QString> names = {
      {"start", QObject::tr("Start")},
      {"import_views", QObject::tr("Import drawings")},
      {"reload_views", QObject::tr("Reload drawings")},
      {"auto_align", QObject::tr("Align drawings")},
      {"reconstruct", QObject::tr("Reconstruct")},
      {"project_textures", QObject::tr("Project textures")},
      {"edit_part", QObject::tr("Choose parts")},
      {"set_depth", QObject::tr("Set depth")},
      {"paint_depth", QObject::tr("Depth brush")},
      {"push_pull", QObject::tr("Push/pull")},
      {"paint_part_mask", QObject::tr("Paint part mask")},
      {"create_part", QObject::tr("New part")},
      {"rename_part", QObject::tr("Rename part")},
      {"mirror", QObject::tr("Mirror")},
      {"transform_part", QObject::tr("Transform part")},
      {"split_part", QObject::tr("Split part")},
      {"merge_parts", QObject::tr("Merge parts")},
      {"delete_part", QObject::tr("Delete part")},
      {"add_volume", QObject::tr("Add volume")},
      {"erase_volume", QObject::tr("Erase volume")},
      {"extrude", QObject::tr("Extrude")},
      {"cut", QObject::tr("Cut")},
      {"sync_part_masks", QObject::tr("Fill part masks")},
      {"set_single_view", QObject::tr("Single drawing")},
      {"set_single_view_depth", QObject::tr("Single-drawing depth")},
  };
  if (const auto it = names.find(op); it != names.end()) return it->second;
  QString t = qs(op);
  t.replace('_', ' ');
  if (!t.isEmpty()) t[0] = t[0].toUpper();
  return t;
}

}  // namespace

HistoryPanel::HistoryPanel(QWidget* parent) : QWidget(parent) {
  setProperty("surface", "panel");
  setAttribute(Qt::WA_StyledBackground, true);
  auto* lay = new QHBoxLayout(this);
  lay->setContentsMargins(theme::space::lg, theme::space::xs, theme::space::lg, theme::space::sm);
  lay->setSpacing(theme::space::xl);
  auto* left = new QVBoxLayout;
  left->setSpacing(theme::space::xs);
  left->addWidget(make_label(tr("LIVE CHECKS"), "section"));
  live_ = new QLabel;
  live_->setTextFormat(Qt::RichText);
  live_->setWordWrap(true);
  live_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  auto* scroll = new QScrollArea;
  scroll->setWidget(live_);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  left->addWidget(scroll, 1);
  auto* right = new QVBoxLayout;
  right->setSpacing(theme::space::xs);
  right->addWidget(make_label(tr("HISTORY"), "section"));
  list_ = new QListWidget;
  list_->setToolTip(tr("Every change is one step. Double-click a step to go back (or forward) to it."));
  right->addWidget(list_, 1);
  lay->addLayout(left, 3);
  lay->addLayout(right, 2);
  connect(list_, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem* it) { emit jumpRequested(static_cast<size_t>(list_->row(it))); });
}

QString HistoryPanel::liveText() const { return live_->text(); }

void HistoryPanel::update_from(const SessionView& v) {
  const theme::Colors& c = theme::colors();
  const json live = inspect_live(v.doc);
  if (live.value("available", false)) {
    auto mark = [&](bool ok, const QString& text) {
      return QStringLiteral("<span style='color:%1'>●</span>&nbsp;%2").arg((ok ? c.pass : c.review).name(), text.toHtmlEscaped());
    };
    QStringList rows;
    const json views = live.value("views", json::object());
    for (const auto& [view, vm] : views.items()) {
      const int px = vm.value("mismatch_px", 0);
      const auto role = view_role_from_string(view);
      const QString name = role ? view_title(*role) : qs(view);
      rows << mark(px == 0, px == 0 ? tr("%1 matches its drawing").arg(name)
                                    : tr("%1: %2 px differ from the drawing").arg(name).arg(px));
    }
    const bool tight = live.value("watertight", false);
    const int nme = live.value("non_manifold_edges", 0), nmv = live.value("non_manifold_vertices", 0);
    rows << mark(tight && nme == 0 && nmv == 0,
                 tight && nme == 0 && nmv == 0 ? tr("Solid and closed (watertight, manifold)")
                                               : tr("Not closed: %1 open/non-manifold edges, %2 vertices").arg(nme).arg(nmv));
    rows << mark(true, tr("%1 pieces · %2 triangles").arg(live.value("shells", 0)).arg(live.value("triangles", 0)));
    const double inferred = live.value("inferred_volume_fraction", 0.0), ambiguous = live.value("ambiguous_volume_fraction", 0.0);
    rows << mark(ambiguous == 0, tr("Estimated %1 · undecided %2").arg(percent(inferred), percent(ambiguous)));
    const json tex = live.value("texture", json::object());
    if (tex.is_object() && tex.contains("seen_fraction")) {
      const double seen = tex.value("seen_fraction", 0.0) + tex.value("manual_fraction", 0.0);
      rows << mark(true, tr("Texture: %1 from the drawings, %2 estimated").arg(percent(seen), percent(1.0 - seen)));
    }
    const json masks = live.value("mask_conflicts", json::array());
    if (!masks.empty()) rows << mark(false, tr("%1 part mask(s) disagree with the geometry").arg(masks.size()));
    rows << QStringLiteral("<span style='color:%1'>%2</span>")
                .arg(c.text_muted.name(), tr("Validate (F6) for the full check").toHtmlEscaped());
    live_->setText(rows.join(QStringLiteral("<br>")));
  } else {
    live_->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                       .arg(c.text_muted.name(), tr("Available once the model is reconstructed.").toHtmlEscaped()));
  }

  const QSignalBlocker block(list_);
  list_->clear();
  for (size_t i = 0; i < v.history.size(); ++i) {
    auto* it = new QListWidgetItem(step_title(v.history[i]));
    if (i == v.history_cursor) {
      QFont f = it->font();
      f.setBold(true);
      it->setFont(f);
      it->setText(it->text() + tr("  ← now"));
    } else if (i > v.history_cursor) {
      it->setForeground(c.text_disabled);  // undone: redo brings it back
    }
    list_->addItem(it);
  }
  if (!v.history.empty()) list_->scrollToItem(list_->item(static_cast<int>(std::min(v.history_cursor, v.history.size() - 1))));
}

}  // namespace p3dgui
