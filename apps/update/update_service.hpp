#pragma once
// Updates from GitHub Releases (docs/UPDATES.md). Nothing is sent but the requests themselves: no account,
// no identifier, no telemetry; the automatic check (once a day) can be turned off in Settings.
//
//   check()     release list -> newest release for the channel -> update-manifest.json + .minisig ->
//               signature checked with the built-in key (release_key.hpp) -> offer()
//   download()  the package (resumable, size + SHA-256) -> unpacked next to the install folder ->
//               every file checked against the signed manifest -> ready(stagedDir)
//   startUpdater()  hands over to 3re-update.exe (run from the temp folder), which swaps the folders after
//               3re closes, runs the self-test and goes back to the previous version if it fails.
// Projects and settings are outside the install folder and never touched.
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <optional>

#include "update/manifest.hpp"
#include "update/minisign.hpp"

class QNetworkAccessManager;

namespace threere::update {

class Download;

class UpdateService : public QObject {
  Q_OBJECT
 public:
  explicit UpdateService(QObject* parent = nullptr);

  // Settings (QSettings "update/*").
  static QString channel();  // "stable" or "alpha"; default: alpha while 3re itself is a pre-release
  static void setChannel(const QString& channel);
  static bool autoCheck();
  static void setAutoCheck(bool on);
  static bool checkDue();  // automatic checks: at most once a day

  // The release folder this program runs from (it holds 3re.exe and RELEASE.json), or empty.
  static QString installRoot();
  // Why this copy cannot update itself (a development build, a folder without RELEASE.json), or empty.
  static QString notUpdatableReason();
  static QString previousVersion();  // the version kept for roll-back ("<install>.previous"), or empty
  // The result 3re-update.exe left for us (then removed), or an empty object.
  static QJsonObject takeLastResult();
  static QString resultFile();

  // Tests: another release list and trust key (never set in the application).
  void setFeedUrl(const QUrl& url) { feed_ = url; }
  void setTrustedKey(const PublicKey& key) { key_ = key; }
  void setCurrentVersion(const QString& version);
  void setInstallRoot(const QString& root) { root_ = root; }

  void check();
  const std::optional<Manifest>& offer() const { return offer_; }
  void download();
  void cancel();
  bool busy() const { return busy_; }
  QString stagedDir() const { return staged_; }
  // Starts 3re-update.exe for the staged release; the caller then quits 3re.
  bool startUpdater(bool relaunch, QString* error);
  bool startRollback(QString* error);

  // The staged tree matches the package's file list exactly (no file missing, changed or extra).
  static QString verifyTree(const QString& dir, const Package& package);
  // Unpacks a package zip into dir, refusing unsafe paths; empty on success.
  static QString unpack(const QString& zip, const QString& dir);

 signals:
  void checked(bool available, const QString& message);
  void progress(qint64 done, qint64 total, const QString& what);
  void ready(const QString& stagedDir);
  void failed(const QString& reason);

 private:
  QString root() const;
  QString workDir() const;  // "<install>/../.3re-update": same volume as the install folder
  void fetchManifest(const ReleaseChoice& choice);
  void finish(bool available, const QString& message);
  void fail(const QString& reason);
  bool launch(const QStringList& args, QString* error);

  QNetworkAccessManager* nam_;
  QUrl feed_;
  std::optional<PublicKey> key_;
  std::optional<Version> current_;
  QString root_, staged_;
  std::optional<Manifest> offer_;
  QPointer<Download> download_;
  bool busy_ = false;
};

}  // namespace threere::update
