#include "studio/update_ui.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "pixel3d/common/version.hpp"
#include "update/model_store.hpp"
#include "update/update_service.hpp"
#include "widgets.hpp"

namespace p3dgui {
using namespace threere::update;

namespace {
QString current_version() {
  const auto v = pixel3d::version();
  return QString::fromUtf8(v.data(), qsizetype(v.size()));
}

QString notes_html(const Manifest& m) {
  QString html = "<p>" + QString::fromStdString(m.notes_en).toHtmlEscaped().replace("\n", "<br>") + "</p>";
  if (!m.notes_ar.empty())
    html += "<hr><p dir=\"rtl\" align=\"right\">" + QString::fromStdString(m.notes_ar).toHtmlEscaped().replace("\n", "<br>") + "</p>";
  return html;
}
}  // namespace

UpdateDialog::UpdateDialog(UpdateService* service, QWidget* parent) : QDialog(parent), service_(service) {
  setAttribute(Qt::WA_DeleteOnClose);
  setObjectName("updateDialog");
  const Manifest& m = *service->offer();
  const QString version = QString::fromStdString(m.version_text);
  const Package* pkg = m.package_for(kPlatform);
  setWindowTitle(tr("3re %1 is available").arg(version));
  auto* col = new QVBoxLayout(this);
  col->addWidget(make_label(tr("3re %1 is available").arg(version), "title", this));
  col->addWidget(make_label(tr("You have %1. What's new:").arg(current_version()), "secondary", this));
  notes_ = new QTextBrowser(this);
  notes_->setObjectName("updateNotes");
  notes_->setOpenExternalLinks(false);
  notes_->setHtml(notes_html(m));
  notes_->setMinimumSize(480, 200);
  col->addWidget(notes_, 1);
  auto* how = make_label(tr("The download (%1) comes from the 3re releases on GitHub. Before anything changes, 3re checks the "
                            "release signature and every file's SHA-256. 3re then restarts; if the new version fails its "
                            "self-test, the current one comes back. Your projects and settings are not touched.")
                             .arg(ModelStore::sizeText(pkg ? qint64(pkg->size) : 0)),
                         "secondary", this);
  how->setWordWrap(true);
  col->addWidget(how);
  bar_ = new QProgressBar(this);
  bar_->hide();
  col->addWidget(bar_);
  status_ = make_label(QString(), "secondary", this);
  status_->setObjectName("updateStatus");
  status_->setWordWrap(true);
  col->addWidget(status_);
  auto* row = new QHBoxLayout;
  skip_ = make_button(tr("Skip this version"), "ghost", {}, this);
  later_ = make_button(tr("Later"), "ghost", {}, this);
  install_ = make_button(tr("Download and install"), "primary", {}, this);
  install_->setObjectName("updateInstall");
  onClose_ = make_button(tr("Install when I close 3re"), "ghost", {}, this);
  restart_ = make_button(tr("Restart now"), "primary", {}, this);
  restart_->setObjectName("updateRestart");
  onClose_->hide();
  restart_->hide();
  row->addWidget(skip_);
  row->addStretch(1);
  for (auto* b : {later_, install_, onClose_, restart_}) row->addWidget(b);
  col->addLayout(row);

  connect(later_, &QPushButton::clicked, this, &QDialog::reject);
  connect(skip_, &QPushButton::clicked, this, [this, version] {
    QSettings().setValue("update/skip_version", version);
    reject();
  });
  connect(install_, &QPushButton::clicked, this, &UpdateDialog::startDownload);
  connect(service_, &UpdateService::progress, this, [this](qint64 done, qint64 total, const QString& what) {
    bar_->setRange(0, total > 0 ? 1000 : 0);
    if (total > 0) bar_->setValue(int(1000 * done / total));
    status_->setText(total > 0 ? QStringLiteral("%1 — %2 / %3").arg(what, ModelStore::sizeText(done), ModelStore::sizeText(total)) : what);
  });
  connect(service_, &UpdateService::ready, this, &UpdateDialog::onReady);
  connect(service_, &UpdateService::failed, this, [this](const QString& reason) {
    bar_->hide();
    status_->setText(reason);
    install_->setEnabled(true);
    install_->setText(tr("Try again"));
    later_->setEnabled(true);
  });
  connect(restart_, &QPushButton::clicked, this, [this] {
    QString error;
    if (!service_->startUpdater(true, &error)) {
      status_->setText(tr("Could not start the update: %1").arg(error));
      return;
    }
    qApp->quit();
  });
  connect(onClose_, &QPushButton::clicked, this, [this] {
    auto* service = service_;
    connect(qApp, &QCoreApplication::aboutToQuit, service, [service] { service->startUpdater(false, nullptr); });
    accept();
  });
}

void UpdateDialog::startDownload() {
  install_->setEnabled(false);
  skip_->setEnabled(false);
  later_->setEnabled(false);
  bar_->show();
  status_->setText(tr("Starting the download…"));
  service_->download();
}

void UpdateDialog::onReady() {
  bar_->hide();
  status_->setText(tr("Downloaded and checked. 3re restarts to finish (about a minute); unsaved work is not saved for you."));
  install_->hide();
  later_->hide();
  skip_->hide();
  onClose_->show();
  restart_->show();
}

ModelDownloadDialog::ModelDownloadDialog(ModelStore* store, QWidget* parent) : QDialog(parent), store_(store) {
  setAttribute(Qt::WA_DeleteOnClose);
  setObjectName("modelDialog");
  setWindowTitle(tr("Download the AI models"));
  auto* col = new QVBoxLayout(this);
  col->addWidget(make_label(tr("Download the AI models"), "title", this));
  auto* intro = make_label(tr("Image to 3D, segmentation and the Agent need these files. They are not part of the 3re download; "
                              "Pixel Art Builder works without them. They are downloaded once, from the sources below, and "
                              "checked (size and SHA-256) before 3re uses them. An interrupted download resumes."),
                           "secondary", this);
  intro->setWordWrap(true);
  col->addWidget(intro);
  QStringList ids;
  qint64 total = 0;
  QString html;
  for (const auto& p : store->missing()) {
    ids << p.id;
    total += p.downloadSize();
    QStringList links;
    for (const auto& u : p.licence_urls) links << QStringLiteral("<a href=\"%1\">%2</a>").arg(u.toHtmlEscaped(), u.toHtmlEscaped());
    html += QStringLiteral("<p><b>%1</b> — %2<br>%3<br>%4: %5<br>%6: %7<br>%8%9</p>")
                .arg(p.title.toHtmlEscaped(), ModelStore::sizeText(p.downloadSize()), p.purpose.toHtmlEscaped(), tr("From"),
                     p.source.toHtmlEscaped(), tr("Licence"), p.licence.toHtmlEscaped(), links.join("<br>"),
                     p.attribution.isEmpty() ? QString() : "<br><i>" + p.attribution.toHtmlEscaped() + "</i>");
  }
  auto* list = new QLabel(html.isEmpty() ? tr("All models are already downloaded.") : html, this);
  list->setObjectName("modelList");
  list->setWordWrap(true);
  list->setTextFormat(Qt::RichText);
  list->setOpenExternalLinks(true);
  col->addWidget(list);
  col->addWidget(make_label(tr("Total: %1. Make sure the drive has that much free space.").arg(ModelStore::sizeText(total)), "secondary", this));
  accept_ = new QCheckBox(tr("I have read these licences and accept them"), this);
  accept_->setObjectName("modelAccept");
  col->addWidget(accept_);
  bar_ = new QProgressBar(this);
  bar_->hide();
  col->addWidget(bar_);
  status_ = make_label(QString(), "secondary", this);
  status_->setWordWrap(true);
  col->addWidget(status_);
  auto* row = new QHBoxLayout;
  notNow_ = make_button(tr("Not now"), "ghost", {}, this);
  cancel_ = make_button(tr("Cancel"), "ghost", {}, this);
  download_ = make_button(tr("Download %1").arg(ModelStore::sizeText(total)), "primary", {}, this);
  download_->setObjectName("modelDownload");
  download_->setEnabled(false);
  cancel_->hide();
  row->addStretch(1);
  for (auto* b : {notNow_, cancel_, download_}) row->addWidget(b);
  col->addLayout(row);
  if (ids.isEmpty()) {
    accept_->hide();
    download_->hide();
    notNow_->setText(tr("Close"));
  }
  connect(accept_, &QCheckBox::toggled, download_, &QPushButton::setEnabled);
  connect(notNow_, &QPushButton::clicked, this, &QDialog::reject);
  connect(cancel_, &QPushButton::clicked, store_, &ModelStore::cancel);
  connect(download_, &QPushButton::clicked, this, [this, ids] {
    accept_->setEnabled(false);
    download_->setEnabled(false);
    notNow_->hide();
    cancel_->show();
    bar_->show();
    store_->download(ids);
  });
  connect(store_, &ModelStore::progress, this, [this](qint64 done, qint64 all, const QString& what) {
    bar_->setRange(0, 1000);
    bar_->setValue(all > 0 ? int(1000 * done / all) : 0);
    status_->setText(QStringLiteral("%1 — %2 / %3").arg(what, ModelStore::sizeText(done), ModelStore::sizeText(all)));
  });
  connect(store_, &ModelStore::finished, this, [this](bool ok, const QString& message) {
    cancel_->hide();
    notNow_->show();
    notNow_->setText(tr("Close"));
    status_->setText(message);
    if (!ok) {
      download_->setText(tr("Try again"));
      download_->setEnabled(true);
    }
  });
}

void start_update_tasks(QWidget* window) {
  const QJsonObject last = UpdateService::takeLastResult();
  if (!last.isEmpty()) {
    const QString status = last.value("status").toString(), from = last.value("from").toString(), to = last.value("to").toString();
    const QString msg = last.value("message").toString();
    if (status == "updated") QMessageBox::information(window, QObject::tr("3re updated"), QObject::tr("3re was updated from %1 to %2.").arg(from, to));
    else if (status == "rolled_back")
      QMessageBox::warning(window, QObject::tr("Update undone"), QObject::tr("3re is back on the version it had before.\n\n%1").arg(msg));
    else QMessageBox::warning(window, QObject::tr("Update not installed"), QObject::tr("The update was not installed; nothing changed.\n\n%1").arg(msg));
  }

  auto* store = new ModelStore(qApp);
  const QString root = ModelStore::defaultRoot();
  if (!root.isEmpty() && store->load(ModelStore::defaultCatalog(), root) && !store->missing().isEmpty() &&
      !QSettings().value("models/offered", false).toBool()) {
    QSettings().setValue("models/offered", true);
    (new ModelDownloadDialog(store, window))->open();
  }

  if (UpdateService::checkDue() && UpdateService::notUpdatableReason().isEmpty()) {
    auto* service = new UpdateService(qApp);
    QObject::connect(service, &UpdateService::checked, window, [service, window](bool available, const QString&) {
      if (!available || !service->offer()) return;
      if (QSettings().value("update/skip_version").toString() == QString::fromStdString(service->offer()->version_text)) return;
      (new UpdateDialog(service, window))->open();
    });
    service->check();
  }
}

}  // namespace p3dgui
