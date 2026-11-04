#include "studio/rigging_page.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "app_controller.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "shell/coming_soon.hpp"
#include "shell/feature_badge.hpp"
#include "widgets.hpp"

namespace p3dgui {

RiggingPage::RiggingPage(MainWindow& pixel, QWidget* parent) : QWidget(parent), pixel_(pixel) {
  stack_ = new QStackedWidget(this);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(stack_);

  stack_->addWidget(new ComingSoonPanel(
      tr("Rigging"),
      tr("Automatic skeletons for generated and imported meshes. Low-Poly Studio characters already get an automatic rig: "
         "build one in the Pixel Art Builder and it appears here."),
      {tr("Joint placement from the picture or the mesh"), tr("Skin weights that keep the generated shape"),
       tr("Checked export of skinned GLB files")}));

  auto* lowpoly = new QWidget;
  auto* col = new QVBoxLayout(lowpoly);
  col->setContentsMargins(48, 36, 48, 36);
  col->setSpacing(theme::space::lg);
  auto* card = new Card(lowpoly);
  card->setMaximumWidth(640);
  auto* l = new QVBoxLayout(card);
  l->setContentsMargins(24, 22, 24, 22);
  l->setSpacing(10);
  auto* head = new QHBoxLayout;
  head->addWidget(make_label(tr("Low-Poly Studio rig"), "display", card));
  head->addWidget(make_feature_badge(Feature::Experimental, card), 0, Qt::AlignVCenter);
  head->addStretch(1);
  l->addLayout(head);
  summary_ = make_label(QString(), "lead", card);
  summary_->setObjectName("riggingSummary");
  summary_->setWordWrap(true);
  l->addWidget(summary_);
  pose_ = new QCheckBox(tr("Preview the test pose"), card);
  pose_->setObjectName("riggingPose");
  pose_->setToolTip(tr("Bends the joints to show how the skin follows the bones (preview only)."));
  l->addWidget(pose_);
  exportRigged_ = make_button(tr("Export Rigged GLB…"), "primary", {}, card);
  exportRigged_->setObjectName("riggingExport");
  l->addWidget(exportRigged_, 0, Qt::AlignLeft);
  col->addWidget(card);
  col->addStretch(1);
  stack_->addWidget(lowpoly);

  connect(pose_, &QCheckBox::toggled, this, [this](bool on) { pixel_.setLowpolyPose(on); });
  // A Low-Poly Studio export is the skinned GLB (mesh + rig + texture), checked on reload.
  connect(exportRigged_, &QPushButton::clicked, this, [this] { pixel_.runAction("export"); });
  refresh();
}

void RiggingPage::refresh() {
  const auto& doc = pixel_.controller().view().doc;
  const pixel3d::LowpolyBuild* build = doc.settings.lowpoly.enabled ? pixel3d::current_lowpoly(doc) : nullptr;
  stack_->setCurrentIndex(build ? 1 : 0);
  if (!build) return;
  summary_->setText(tr("%1 parts are skinned to an automatic skeleton (chains such as arms and legs bend on joint loops). "
                       "The rig is exported in the skinned GLB; check the pose before you use it in a game.")
                        .arg(build->result.parts.size()));
  pose_->setChecked(pixel_.lowpolyPose());
}

}  // namespace p3dgui
