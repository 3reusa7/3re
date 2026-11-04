#pragma once
// The update and first-run model dialogs of the studio (docs/UPDATES.md). Nothing downloads or installs
// without the person's confirmation: the update dialog shows "what's new" first, the model dialog shows
// every pack's size, source and licence first.
#include <QDialog>

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTextBrowser;

namespace threere::update {
class ModelStore;
class UpdateService;
}  // namespace threere::update

namespace p3dgui {

class UpdateDialog : public QDialog {
  Q_OBJECT
 public:
  // service must hold an offer (UpdateService::checked(true, …)).
  UpdateDialog(threere::update::UpdateService* service, QWidget* parent = nullptr);

 private:
  void startDownload();
  void onReady();
  threere::update::UpdateService* service_;
  QTextBrowser* notes_;
  QProgressBar* bar_;
  QLabel* status_;
  QPushButton *install_, *later_, *skip_, *restart_, *onClose_;
};

class ModelDownloadDialog : public QDialog {
  Q_OBJECT
 public:
  // Lists the packs that are not downloaded yet; empty store = nothing to do (the dialog says so).
  ModelDownloadDialog(threere::update::ModelStore* store, QWidget* parent = nullptr);

 private:
  threere::update::ModelStore* store_;
  QCheckBox* accept_;
  QProgressBar* bar_;
  QLabel* status_;
  QPushButton *download_, *notNow_, *cancel_;
};

// After the main window shows: the result of the last update, the daily update check (when on) and,
// on the first start of a release without models, the offer to download them.
void start_update_tasks(QWidget* window);

}  // namespace p3dgui
