#include "studio/export_page.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include "app_controller.hpp"
#include "icons.hpp"
#include "main_window.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "studio/image3d_workspace.hpp"
#include "widgets.hpp"

namespace p3dgui {

ExportPage::ExportPage(QWidget* parent) : QWidget(parent) {
  auto* col = new QVBoxLayout(this);
  col->setContentsMargins(48, 32, 48, 32);
  col->setSpacing(theme::space::lg);

  empty_ = new Card(this);
  empty_->setMaximumWidth(640);
  auto* el = new QVBoxLayout(empty_);
  el->setContentsMargins(24, 22, 24, 22);
  el->addWidget(make_label(tr("Nothing to export yet"), "title", empty_));
  auto* et = make_label(tr("Generate a model in Image to 3D, or build one in the Pixel Art Builder. It appears here, ready to save "
                           "as a GLB for Godot, Unity, Roblox, Blender and other tools."),
                        "secondary", empty_);
  et->setWordWrap(true);
  el->addWidget(et);
  auto* go = make_button(tr("Go to Image to 3D"), "primary", {}, empty_);
  go->setObjectName("exportGoImage");
  el->addWidget(go, 0, Qt::AlignLeft);
  connect(go, &QPushButton::clicked, this, &ExportPage::goToImage);
  col->addWidget(empty_);

  ai_card_ = new Card(this);
  ai_card_->setMaximumWidth(760);
  auto* al = new QVBoxLayout(ai_card_);
  al->setContentsMargins(24, 22, 24, 22);
  al->setSpacing(10);
  al->addWidget(make_label(tr("Generated model (Image to 3D)"), "section", ai_card_));
  auto* ah = new QHBoxLayout;
  ai_name_ = make_label(QString(), "title", ai_card_);
  ah->addWidget(ai_name_);
  ai_chip_ = make_chip(QString(), theme::Tone::Neutral, ai_card_);
  ai_chip_->setObjectName("exportAiChip");
  ah->addWidget(ai_chip_);
  ah->addStretch(1);
  al->addLayout(ah);
  ai_text_ = make_label(QString(), "secondary", ai_card_);
  ai_text_->setWordWrap(true);
  al->addWidget(ai_text_);
  auto* ab = new QHBoxLayout;
  ai_export_ = make_button(tr("Export GLB…"), "primary", icon(Icon::Export, IconStyle::OnAccent), ai_card_);
  ai_export_->setObjectName("exportAiGlb");
  ai_export_->setToolTip(tr("Writes a GLB, reads it back and compares it with the model before reporting success."));
  ab->addWidget(ai_export_);
  ai_folder_ = make_button(tr("Show in Folder"), "ghost", icon(Icon::Folder), ai_card_);
  ai_folder_->setEnabled(false);
  ab->addWidget(ai_folder_);
  ab->addStretch(1);
  al->addLayout(ab);
  ai_status_ = make_label(QString(), nullptr, ai_card_);
  ai_status_->setObjectName("exportAiStatus");
  ai_status_->setWordWrap(true);
  al->addWidget(ai_status_);
  col->addWidget(ai_card_);

  pixel_card_ = new Card(this);
  pixel_card_->setMaximumWidth(760);
  auto* pl = new QVBoxLayout(pixel_card_);
  pl->setContentsMargins(24, 22, 24, 22);
  pl->setSpacing(10);
  pl->addWidget(make_label(tr("Pixel Art Builder model"), "section", pixel_card_));
  pixel_text_ = make_label(QString(), "secondary", pixel_card_);
  pixel_text_->setWordWrap(true);
  pl->addWidget(pixel_text_);
  auto* pe = make_button(tr("Export Model…"), "primary", icon(Icon::Export, IconStyle::OnAccent), pixel_card_);
  pe->setObjectName("exportPixel");
  pe->setToolTip(tr("GLB, glTF or OBJ with presets for Godot, Unity and Roblox (Low-Poly Studio: skinned GLB)."));
  pl->addWidget(pe, 0, Qt::AlignLeft);
  col->addWidget(pixel_card_);
  col->addStretch(1);

  job_ = new QFutureWatcher<QString>(this);
  connect(ai_export_, &QPushButton::clicked, this, &ExportPage::exportGenerated);
  connect(pe, &QPushButton::clicked, this, [this] { if (pixel_) pixel_->runAction("export"); });
  connect(ai_folder_, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(last_export_).absolutePath()));
  });
  connect(job_, &QFutureWatcher<QString>::finished, this, [this] {
    const QString error = job_->result();
    ai_export_->setEnabled(true);
    ai_folder_->setEnabled(error.isEmpty());
    ai_status_->setText(error.isEmpty() ? tr("Saved and re-imported: %1 · geometry, texture and validation report retained.").arg(QDir::toNativeSeparators(last_export_))
                                        : error);
    theme::set_prop(ai_status_, "tone", error.isEmpty() ? "pass" : "fail");
  });
  refresh();
}

void ExportPage::setSources(Image3DWorkspace* ai, MainWindow* pixel) {
  ai_ = ai;
  pixel_ = pixel;
  refresh();
}

void ExportPage::refresh() {
  const bool ai = ai_ && ai_->hasResult();
  bool pixel = false;
  if (pixel_) {
    const auto& doc = pixel_->controller().view().doc;
    pixel = doc.settings.lowpoly.enabled ? pixel3d::current_lowpoly(doc) != nullptr : doc.solid != nullptr;
  }
  ai_card_->setVisible(ai);
  pixel_card_->setVisible(pixel);
  empty_->setVisible(!ai && !pixel);
  if (ai) {
    const auto v = ai_->backend().asset().validation;
    const auto tone = v == p3dasset::Validation::Pass ? theme::Tone::Pass : v == p3dasset::Validation::Unsafe ? theme::Tone::Fail : theme::Tone::Review;
    ai_name_->setText(ai_->projectName().isEmpty() ? tr("Generated model") : ai_->projectName());
    set_chip(ai_chip_, QString::fromUtf8(p3dasset::validation_name(v)).toUpper(), tone);
    const auto& a = ai_->backend().asset();
    ai_text_->setText(QStringLiteral("%1\n%2")
                          .arg(a.geometry_only ? tr("Geometry only (no texture).") : tr("With colours from the picture (front projection; back and sides approximate)."),
                               v == p3dasset::Validation::Pass ? tr("Validated: ready for game engines.")
                                                               : tr("Has reported issues: you will be asked to confirm before saving.")));
  }
  if (pixel) {
    const auto& doc = pixel_->controller().view().doc;
    pixel_text_->setText(doc.settings.lowpoly.enabled ? tr("Low-Poly Studio character: skinned GLB with rig and texture.")
                                                      : tr("Solid model from your drawings: GLB, glTF or OBJ."));
  }
}

void ExportPage::exportGenerated() {
  if (!ai_ || !ai_->hasResult()) return;
  exportAsset(ai_->backend().asset(), ai_->projectName(), ai_->project() ? ai_->project()->dir : QString());
}

void ExportPage::exportAsset(const p3dasset::MeshAsset& asset, const QString& name, const QString& folder) {
  if (job_->isRunning()) return;
  if (asset.validation != p3dasset::Validation::Pass) {
    const QString text = tr("This model is %1. Its issues are listed in the validation report saved next to the GLB.\n\nSave it with these reported issues?")
                             .arg(QString::fromUtf8(p3dasset::validation_name(asset.validation)));
    const bool ok = acknowledge_ ? acknowledge_(text)
                                 : QMessageBox::warning(this, tr("Model needs review"), text, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
    if (!ok) return;
  }
  const QString base = name.isEmpty() ? QStringLiteral("model") : name;
  const QString suggestion = (folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : folder) + "/" + base + ".glb";
  const QString file = ask_path_ ? ask_path_(suggestion) : QFileDialog::getSaveFileName(this, tr("Export GLB"), suggestion, tr("glTF binary (*.glb)"));
  if (file.isEmpty()) return;
  last_export_ = file;
  ai_export_->setEnabled(false);
  ai_status_->setText(tr("Exporting and re-importing…"));
  theme::set_prop(ai_status_, "tone", "");
  job_->setFuture(QtConcurrent::run([asset, file] {
    QString error;
    p3dasset::TrellisBackend::exportAsset(asset, file, true, error);
    return error;
  }));
}

}  // namespace p3dgui
