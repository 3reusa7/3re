#include "studio/settings_page.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPixmap>
#include <QPointer>
#include <QStyle>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include <QApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <thread>

#include "agent/agent_godot.hpp"
#include "backends/retexture_batch.hpp"
#include "backends/trellis_backend.hpp"
#include "icons.hpp"
#include "pixel3d/common/version.hpp"
#include "project/project_store.hpp"
#include "studio/update_ui.hpp"
#include "update/model_store.hpp"
#include "update/update_service.hpp"
#include "widgets.hpp"

namespace p3dgui {
namespace {
QVBoxLayout* section(QVBoxLayout* col, QWidget* parent, const QString& title) {
  auto* c = new Card(parent);
  c->setMaximumWidth(760);
  auto* l = new QVBoxLayout(c);
  l->setContentsMargins(20, 18, 20, 18);
  l->setSpacing(10);
  l->addWidget(make_label(title, "section", c));
  col->addWidget(c);
  return l;
}
}  // namespace

const QList<double>& SettingsPage::scales() {
  static const QList<double> s{0, 1.0, 1.25, 1.5, 1.75, 2.0};
  return s;
}

SettingsPage::SettingsPage(QWidget* parent) : QWidget(parent) {
  auto* inner = new QWidget;
  auto* col = new QVBoxLayout(inner);
  col->setContentsMargins(48, 28, 48, 28);
  col->setSpacing(theme::space::lg);
  QSettings s;

  auto* gen = section(col, inner, tr("Generation"));
  auto* gf = new QFormLayout;
  const QString setup = p3dasset::find_backend_config();
  auto* setupLabel = make_label(setup.isEmpty() ? tr("Not found — generation is unavailable") : QDir::toNativeSeparators(QDir::cleanPath(setup)), "secondary", inner);
  setupLabel->setObjectName("settingsSetup");
  setupLabel->setWordWrap(true);
  setupLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  gf->addRow(tr("Setup file"), setupLabel);
  projects_ = new QLineEdit(default_projects_dir(), inner);
  projects_->setObjectName("settingsProjectsDir");
  projects_->setToolTip(tr("New Image-to-3D projects are created here."));
  auto* pick = make_button(tr("Choose…"), "ghost", icon(Icon::Folder), inner);
  auto* pr = new QHBoxLayout;
  pr->addWidget(projects_, 1);
  pr->addWidget(pick);
  gf->addRow(tr("Projects folder"), pr);
  gen->addLayout(gf);
  connect(pick, &QPushButton::clicked, this, [this] {
    const auto d = QFileDialog::getExistingDirectory(this, tr("Projects folder"), projects_->text());
    if (!d.isEmpty()) projects_->setText(d);
  });
  connect(projects_, &QLineEdit::editingFinished, this, [this] { QSettings().setValue("projects/dir", projects_->text()); });
  connect(projects_, &QLineEdit::textChanged, this, [this](const QString& t) { QSettings().setValue("projects/dir", t); });

  // 2.10.0: Godot for the agent's delivery check. Saved in agent-settings.json, which 3re-chat and 3re-agent read.
  auto* gd = section(col, inner, tr("Delivery check (Godot)"));
  auto* gdText = make_label(tr("The agent imports every delivered model in Godot 4 (headless) and measures it there; without Godot no "
                               "model can PASS. 3re finds an installed Godot by itself; choose its console executable here if it is "
                               "somewhere else."),
                            "secondary", inner);
  gdText->setWordWrap(true);
  gd->addWidget(gdText);
  godot_ = new QLineEdit(QString::fromStdWString(p3dagent::saved_godot_path().wstring()), inner);
  godot_->setObjectName("settingsGodot");
  godot_->setPlaceholderText(tr("Automatic"));
  auto* gdPick = make_button(tr("Choose…"), "ghost", icon(Icon::Folder), inner);
  auto* gdCheck = make_button(tr("Check"), "ghost", {}, inner);
  gdCheck->setObjectName("settingsGodotCheck");
  auto* gdRow = new QHBoxLayout;
  gdRow->addWidget(godot_, 1);
  gdRow->addWidget(gdPick);
  gdRow->addWidget(gdCheck);
  gd->addLayout(gdRow);
  godot_status_ = make_label(QString(), "secondary", inner);
  godot_status_->setObjectName("settingsGodotStatus");
  godot_status_->setWordWrap(true);
  godot_status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  gd->addWidget(godot_status_);
  connect(gdPick, &QPushButton::clicked, this, [this] {
    const auto f = QFileDialog::getOpenFileName(this, tr("Godot 4 console executable"), QFileInfo(godot_->text()).absolutePath(),
                                                tr("Godot (Godot*.exe godot*.exe);;Programs (*.exe)"));
    if (f.isEmpty()) return;
    godot_->setText(QDir::toNativeSeparators(f));
    saveGodot();
  });
  connect(godot_, &QLineEdit::editingFinished, this, &SettingsPage::saveGodot);
  connect(gdCheck, &QPushButton::clicked, this, &SettingsPage::refreshGodot);
  refreshGodot();

  // Re-texture all saved models: the current texture engine once more on every saved generation that still
  // has TRELLIS's texture next to it. New files only (textured-v2.glb); projects then open with them.
  auto* rt = section(col, inner, tr("Saved models"));
  auto* rtText = make_label(tr("Re-texture all saved models with the current texture engine (sharper picture, no smears on steep "
                               "surfaces, more texture space for faces and hands). Geometry is never changed and no file is "
                               "overwritten: each result is saved as textured-v2.glb next to its run, with its report."),
                            "secondary", inner);
  rtText->setWordWrap(true);
  rt->addWidget(rtText);
  auto* rtRow = new QHBoxLayout;
  retex_ = make_button(tr("Re-texture all saved models"), "primary", {}, inner);
  retex_->setObjectName("settingsRetextureAll");
  retex_cancel_ = make_button(tr("Cancel"), "ghost", icon(Icon::Close), inner);
  retex_cancel_->setObjectName("settingsRetextureCancel");
  retex_cancel_->setEnabled(false);
  rtRow->addWidget(retex_);
  rtRow->addWidget(retex_cancel_);
  rtRow->addStretch(1);
  rt->addLayout(rtRow);
  retex_bar_ = new QProgressBar(inner);
  retex_bar_->setObjectName("settingsRetextureProgress");
  retex_bar_->setRange(0, 1);
  retex_bar_->setValue(0);
  retex_bar_->hide();
  rt->addWidget(retex_bar_);
  retex_text_ = make_label(QString(), "secondary", inner);
  retex_text_->setObjectName("settingsRetextureStatus");
  retex_text_->setWordWrap(true);
  rt->addWidget(retex_text_);
  batch_ = new p3dasset::RetextureBatch(this);
  connect(retex_, &QPushButton::clicked, this, &SettingsPage::startRetextureAll);
  connect(retex_cancel_, &QPushButton::clicked, batch_, &p3dasset::RetextureBatch::cancel);
  connect(batch_, &p3dasset::RetextureBatch::progress, this, [this](int done, int total, const QString& text) {
    retex_bar_->setRange(0, std::max(1, total));
    retex_bar_->setValue(done);
    retex_text_->setText(text);
  });
  connect(batch_, &p3dasset::RetextureBatch::finished, this, [this](bool cancelled, const QJsonObject& summary) {
    retex_->setEnabled(true);
    retex_cancel_->setEnabled(false);
    int ok = 0, other = 0;
    for (const auto& r : summary.value("runs").toArray()) (r.toObject().value("status").toString() == "OK" ? ok : other)++;
    retex_text_->setText(cancelled ? tr("Cancelled: %1 re-textured, the rest unchanged.").arg(ok)
                                   : summary.value("status").toString() == "ERROR" ? summary.value("message").toString()
                                   : tr("%1 model(s) re-textured, %2 skipped or kept. Reports are next to each run (textured-v2.glb.texture.json).").arg(ok).arg(other));
  });

  auto* disp = section(col, inner, tr("Display"));
  auto* df = new QFormLayout;
  scale_ = new QComboBox(inner);
  scale_->setObjectName("settingsScale");
  scale_->addItems({tr("Follow Windows"), tr("100 %"), tr("125 %"), tr("150 %"), tr("175 %"), tr("200 %")});
  scale_->setCurrentIndex(std::max(0, int(scales().indexOf(s.value("ui/scale", 0.0).toDouble()))));
  df->addRow(tr("Interface size"), scale_);
  disp->addLayout(df);
  software_ = new QCheckBox(tr("Use the software 3D view (for graphics drivers without OpenGL 3.3)"), inner);
  software_->setObjectName("settingsSoftwareView");
  software_->setChecked(s.value("ui/software_view", false).toBool());
  disp->addWidget(software_);
  auto* restart = make_label(tr("Interface size and the 3D view take effect the next time 3re starts."), "secondary", inner);
  disp->addWidget(restart);
  connect(scale_, &QComboBox::currentIndexChanged, this, [](int i) { QSettings().setValue("ui/scale", scales().value(i)); });
  connect(software_, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("ui/software_view", on); });

  auto* adv = section(col, inner, tr("Options"));
  advanced_ = new QCheckBox(tr("Show advanced options (seed, setup file, saved results, diagnostic captures)"), inner);
  advanced_->setObjectName("settingsAdvanced");
  advanced_->setChecked(s.value("ui/advanced", false).toBool());
  adv->addWidget(advanced_);
  connect(advanced_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue("ui/advanced", on);
    emit advancedChanged(on);
  });

  // Updates (docs/UPDATES.md): GitHub Releases, signed; nothing installs without a confirmation.
  using threere::update::ModelStore;
  using threere::update::UpdateService;
  auto* up = section(col, inner, tr("Updates"));
  auto* upService = new UpdateService(this);
  auto* uf = new QFormLayout;
  auto* channel = new QComboBox(inner);
  channel->setObjectName("settingsUpdateChannel");
  channel->addItem(tr("Stable — tested releases"), "stable");
  channel->addItem(tr("Alpha — newest features and fixes first"), "alpha");
  channel->setCurrentIndex(std::max(0, channel->findData(UpdateService::channel())));
  uf->addRow(tr("Channel"), channel);
  up->addLayout(uf);
  auto* autoCheck = new QCheckBox(tr("Check for updates once a day"), inner);
  autoCheck->setObjectName("settingsUpdateAuto");
  autoCheck->setChecked(UpdateService::autoCheck());
  up->addWidget(autoCheck);
  // Explicit lines and a minimum height: a wrapped two-line label got one line's height in this card.
  auto* privacy = make_label(tr("The check only asks GitHub for the list of 3re releases.\n"
                                "No account, no identifier and no usage data is sent. Updates install only after you confirm."),
                             "secondary", inner);
  privacy->setMinimumHeight(privacy->sizeHint().height());
  up->addWidget(privacy);
  auto* upRow = new QHBoxLayout;
  auto* checkNow = make_button(tr("Check now"), "ghost", {}, inner);
  checkNow->setObjectName("settingsUpdateCheck");
  auto* rollback = make_button(tr("Go back to %1").arg(UpdateService::previousVersion()), "ghost", {}, inner);
  rollback->setObjectName("settingsUpdateRollback");
  rollback->setVisible(!UpdateService::previousVersion().isEmpty());
  upRow->addWidget(checkNow);
  upRow->addWidget(rollback);
  upRow->addStretch(1);
  up->addLayout(upRow);
  auto* upStatus = make_label(QString(), "secondary", inner);
  upStatus->setObjectName("settingsUpdateStatus");
  upStatus->setWordWrap(true);
  up->addWidget(upStatus);
  if (const QString why = UpdateService::notUpdatableReason(); !why.isEmpty()) {
    checkNow->setEnabled(false);
    upStatus->setText(why);
  }
  connect(channel, &QComboBox::currentIndexChanged, this, [channel](int) { UpdateService::setChannel(channel->currentData().toString()); });
  connect(autoCheck, &QCheckBox::toggled, this, [](bool on) { UpdateService::setAutoCheck(on); });
  connect(checkNow, &QPushButton::clicked, this, [upService, upStatus] {
    upStatus->setText(tr("Checking…"));
    upService->check();
  });
  connect(upService, &UpdateService::checked, this, [this, upService, upStatus](bool available, const QString& message) {
    upStatus->setText(message);
    if (available) (new UpdateDialog(upService, this))->open();
  });
  connect(upService, &UpdateService::failed, upStatus, &QLabel::setText);
  connect(rollback, &QPushButton::clicked, this, [this, upStatus] {
    if (QMessageBox::question(this, tr("Go back to the previous version"),
                              tr("3re closes, puts %1 back and starts again. Your projects and settings are not touched.")
                                  .arg(UpdateService::previousVersion())) != QMessageBox::Yes)
      return;
    QString error;
    if (UpdateService(this).startRollback(&error)) qApp->quit();
    else upStatus->setText(error);
  });

  // AI models: downloaded on first use (app/models.json), each checked by size and SHA-256.
  auto* ai = section(col, inner, tr("AI models"));
  auto* store = new ModelStore(this);
  auto* aiStatus = make_label(QString(), "secondary", inner);
  aiStatus->setObjectName("settingsModelsStatus");
  aiStatus->setWordWrap(true);
  ai->addWidget(aiStatus);
  auto* aiRow = new QHBoxLayout;
  auto* getModels = make_button(tr("Download missing models…"), "ghost", {}, inner);
  getModels->setObjectName("settingsModelsDownload");
  auto* verifyModels = make_button(tr("Check files"), "ghost", {}, inner);
  verifyModels->setObjectName("settingsModelsVerify");
  aiRow->addWidget(getModels);
  aiRow->addWidget(verifyModels);
  aiRow->addStretch(1);
  ai->addLayout(aiRow);
  const QString root = ModelStore::defaultRoot();
  QString catalogError;
  const bool haveStore = !root.isEmpty() && store->load(ModelStore::defaultCatalog(), root, &catalogError);
  auto describe = [store](bool deep) {
    QStringList lines;
    for (const auto& p : store->packs()) {
      QString detail;
      switch (store->state(p, deep, &detail)) {
        case threere::update::PackState::Ready: lines << tr("%1: ready").arg(p.title); break;
        case threere::update::PackState::Missing: lines << tr("%1: not downloaded yet (%2)").arg(p.title, ModelStore::sizeText(p.downloadSize())); break;
        case threere::update::PackState::Partial: lines << tr("%1: incomplete, download again to finish").arg(p.title); break;
        case threere::update::PackState::Damaged: lines << tr("%1: damaged (%2), download again").arg(p.title, detail); break;
      }
    }
    return lines.join("\n");
  };
  if (!haveStore) {
    aiStatus->setText(root.isEmpty() ? tr("Development build: the models come from .toolchain (tools/stores.ps1).") : catalogError);
    getModels->setEnabled(false);
    verifyModels->setEnabled(false);
  } else {
    aiStatus->setText(describe(false));
  }
  connect(getModels, &QPushButton::clicked, this, [this, store] { (new ModelDownloadDialog(store, this))->open(); });
  connect(store, &ModelStore::finished, aiStatus, [aiStatus, describe] { aiStatus->setText(describe(false)); });
  connect(verifyModels, &QPushButton::clicked, this, [aiStatus, verifyModels, describe] {
    verifyModels->setEnabled(false);
    aiStatus->setText(tr("Checking every file's SHA-256 (about a minute for 5 GB)…"));
    auto* watcher = new QFutureWatcher<QString>(aiStatus);
    connect(watcher, &QFutureWatcher<QString>::finished, aiStatus, [watcher, aiStatus, verifyModels] {
      aiStatus->setText(watcher->result());
      verifyModels->setEnabled(true);
      watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([describe] { return describe(true); }));
  });

  auto* about = section(col, inner, tr("About 3re"));
  const auto v = pixel3d::version();
  auto* about_head = new QHBoxLayout;
  about_head->setSpacing(theme::space::md);
  auto* about_mark = new QLabel(inner);
  about_mark->setPixmap(QPixmap(QStringLiteral(":/3re-mark.png")));
  about_mark->setScaledContents(true);
  about_mark->setFixedSize(40, 40);
  about_mark->setAccessibleName(tr("3re logo"));
  about_head->addWidget(about_mark);
  about_head->addWidget(make_label(tr("3re %1 — images and pixel art to validated 3D models").arg(QString::fromUtf8(v.data(), qsizetype(v.size()))), "title", inner), 1);
  about->addLayout(about_head);
  auto* text = make_label(tr("Runs offline. TRELLIS.2 (Q4, 512) generates on an NVIDIA GPU with 4 GB; SAM 2.1-tiny segments on the CPU. "
                             "Pictures, drawings and originals are never uploaded or modified. Third-party components and their "
                             "licences: THIRD-PARTY-NOTICES.txt and the LICENSES folder. Qt is used under the LGPL-3.0."),
                          "secondary", inner);
  text->setWordWrap(true);
  about->addWidget(text);
  auto* links = new QHBoxLayout;
  auto* lic = make_button(tr("Open Licences"), "ghost", {}, inner);
  auto* data = make_button(tr("Open Projects Folder"), "ghost", icon(Icon::Folder), inner);
  links->addWidget(lic);
  links->addWidget(data);
  links->addStretch(1);
  about->addLayout(links);
  connect(lic, &QPushButton::clicked, this, [] {
    QDir d(QCoreApplication::applicationDirPath());
    const QString path = QFileInfo::exists(d.filePath("../LICENSES")) ? d.filePath("../LICENSES") : d.filePath("LICENSES");
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  });
  connect(data, &QPushButton::clicked, this, [this] {
    QDir().mkpath(projects_->text());
    QDesktopServices::openUrl(QUrl::fromLocalFile(projects_->text()));
  });
  diagnostics_ = section(col, inner, tr("Diagnostics"));
  diag_text_ = make_label(tr("Segmentation timings appear here after you use it."), "secondary", inner);
  diag_text_->setObjectName("settingsDiagnostics");
  diag_text_->setWordWrap(true);
  diagnostics_->addWidget(diag_text_);
  col->addStretch(1);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidget(inner);
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->addWidget(scroll);
}

void SettingsPage::setDiagnostics(const QString& text) { diag_text_->setText(text); }

void SettingsPage::saveGodot() {
  const QString text = godot_->text().trimmed();
  std::string error;
  if (QString::fromStdWString(p3dagent::saved_godot_path().wstring()) != text &&
      !p3dagent::save_godot_path(std::filesystem::path(text.toStdWString()), &error)) {
    godot_status_->setText(QString::fromStdString(error));
    return;
  }
  refreshGodot();
}

void SettingsPage::refreshGodot() {
  godot_status_->setText(tr("Looking for Godot 4…"));
  godot_status_->setProperty("tone", "neutral");
  // Off the UI thread: the search runs `godot --version` on each candidate.
  QPointer<QLabel> label = godot_status_;
  std::thread([label] {
    const auto g = p3dagent::locate_godot();
    const QString text = g.found() ? tr("Ready: Godot %1 (%2) — %3")
                                         .arg(QString::fromStdString(g.version), QString::fromStdString(g.source),
                                              QDir::toNativeSeparators(QString::fromStdWString(g.exe.wstring())))
                                   : QString::fromStdString(p3dagent::describe_godot(g));
    const bool ok = g.found();
    QMetaObject::invokeMethod(qApp, [label, text, ok] {
      if (!label) return;
      label->setProperty("tone", ok ? "pass" : "fail");
      label->setText(text);
      label->style()->unpolish(label);
      label->style()->polish(label);
    });
  }).detach();
}

bool SettingsPage::retextureRunning() const { return batch_ && batch_->running(); }

void SettingsPage::startRetextureAll() {
  if (batch_->running()) return;
  const auto jobs = p3dasset::RetextureBatch::scan(p3dasset::RetextureBatch::defaultRoots());
  if (jobs.isEmpty()) {
    retex_text_->setText(tr("No saved model with TRELLIS's texture (native.post) was found in the projects and results folders."));
    return;
  }
  retex_->setEnabled(false);
  retex_cancel_->setEnabled(true);
  retex_bar_->show();
  retex_bar_->setRange(0, int(jobs.size()));
  retex_bar_->setValue(0);
  retex_text_->setText(tr("%1 saved model(s) found; re-texturing in the background…").arg(jobs.size()));
  if (!batch_->start(jobs, QDir(QCoreApplication::applicationDirPath()).filePath("pixel3d-auto-texture.exe"))) {
    retex_->setEnabled(true);
    retex_cancel_->setEnabled(false);
  }
}

}  // namespace p3dgui
