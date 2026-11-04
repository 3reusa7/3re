#include "update/update_service.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <cstdio>
#include <stdexcept>

#include "miniz.h"
#include "pixel3d/common/version.hpp"
#include "update/download.hpp"
#include "update/release_key.hpp"
#include "update/swap.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace threere::update {
namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

QString current_text() {
  const auto v = pixel3d::version();
  return QString::fromUtf8(v.data(), qsizetype(v.size()));
}

size_t write_to_file(void* opaque, mz_uint64, const void* buf, size_t n) {
  auto* f = static_cast<QFile*>(opaque);
  return f->write(static_cast<const char*>(buf), qint64(n)) == qint64(n) ? n : 0;
}

#ifdef Q_OS_WIN
std::wstring quote(const QString& arg) {
  // Command-line quoting for CommandLineToArgvW: backslashes before a quote are doubled.
  std::wstring a = arg.toStdWString(), out = L"\"";
  int slashes = 0;
  for (const wchar_t c : a) {
    if (c == L'\\') { ++slashes; continue; }
    if (c == L'"') out.append(std::size_t(slashes * 2 + 1), L'\\');
    else out.append(std::size_t(slashes), L'\\');
    slashes = 0;
    out.push_back(c);
  }
  out.append(std::size_t(slashes * 2), L'\\');
  return out + L"\"";
}
#endif

}  // namespace

UpdateService::UpdateService(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {
  feed_ = QUrl(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(QString::fromLatin1(kReleaseRepository)));
  key_ = parse_public_key(kReleasePublicKey);
  current_ = parse_version(current_text().toStdString());
}

void UpdateService::setCurrentVersion(const QString& version) { current_ = parse_version(version.toStdString()); }

QString UpdateService::channel() {
  const QString fallback = current_text().contains('-') ? QStringLiteral("alpha") : QStringLiteral("stable");
  const QString c = QSettings().value("update/channel", fallback).toString();
  return c == "alpha" || c == "stable" ? c : fallback;
}

void UpdateService::setChannel(const QString& channel) { QSettings().setValue("update/channel", channel); }
bool UpdateService::autoCheck() { return QSettings().value("update/auto_check", true).toBool(); }
void UpdateService::setAutoCheck(bool on) { QSettings().setValue("update/auto_check", on); }

bool UpdateService::checkDue() {
  if (!autoCheck()) return false;
  const auto last = QDateTime::fromString(QSettings().value("update/last_check").toString(), Qt::ISODate);
  return !last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) > 20 * 3600;
}

QString UpdateService::installRoot() {
  const QDir app(QCoreApplication::applicationDirPath());
  const QString root = QDir::cleanPath(app.filePath(".."));
  if (QFileInfo(QDir(root).filePath("3re.exe")).isFile() && QFileInfo(QDir(root).filePath("RELEASE.json")).isFile()) return root;
  return {};
}

QString UpdateService::notUpdatableReason() {
  const QString root = installRoot();
  if (root.isEmpty()) return tr("This copy of 3re is not an installed release (for example a development build), so it does not update itself.");
  if (!release_files(std::filesystem::path(root.toStdWString())))
    return tr("RELEASE.json is missing or damaged, so 3re cannot tell its own files from yours. Download the new version by hand.");
  if (!QFileInfo(QDir::cleanPath(root + "/..")).isWritable())
    return tr("The folder that holds 3re cannot be written without administrator rights. Update by hand, or install 3re in your user folder.");
  return {};
}

QString UpdateService::previousVersion() {
  const QString root = installRoot();
  if (root.isEmpty()) return {};
  const auto prev = std::filesystem::path((root + ".previous").toStdWString());
  if (!release_files(prev)) return {};
  return qs(release_version(prev));
}

QString UpdateService::resultFile() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("3re/update-result.json");
}

QJsonObject UpdateService::takeLastResult() {
  QFile f(resultFile());
  if (!f.open(QIODevice::ReadOnly)) return {};
  const auto o = QJsonDocument::fromJson(f.readAll()).object();
  f.close();
  f.remove();
  return o;
}

QString UpdateService::root() const { return root_.isEmpty() ? installRoot() : root_; }
QString UpdateService::workDir() const { return QDir::cleanPath(root() + "/../.3re-update"); }

void UpdateService::finish(bool available, const QString& message) {
  busy_ = false;
  emit checked(available, message);
}

void UpdateService::fail(const QString& reason) {
  busy_ = false;
  emit failed(reason);
}

void UpdateService::check() {
  if (busy_) return;
  if (!current_ || !key_) return fail(tr("This build has no valid version or update key."));
  busy_ = true;
  offer_.reset();
  QSettings().setValue("update/last_check", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  auto* f = new Fetch(nam_, this);
  connect(f, &Fetch::done, this, [this, f](bool ok, const QByteArray& body, const QString& error) {
    f->deleteLater();
    if (!ok) return fail(tr("Could not reach GitHub to check for updates (%1).").arg(error));
    const auto choice = choose_release(body.toStdString(), *current_, channel().toStdString());
    if (!choice) return finish(false, tr("3re %1 is up to date (%2 channel).").arg(current_text(), channel()));
    fetchManifest(*choice);
  });
  f->get(feed_, 4 * 1024 * 1024, "application/vnd.github+json");
}

void UpdateService::fetchManifest(const ReleaseChoice& choice) {
  auto* m = new Fetch(nam_, this);
  connect(m, &Fetch::done, this, [this, m, choice](bool ok, const QByteArray& manifest, const QString& error) {
    m->deleteLater();
    if (!ok) return fail(tr("Could not download the update description of %1 (%2).").arg(qs(choice.tag), error));
    auto* s = new Fetch(nam_, this);
    connect(s, &Fetch::done, this, [this, s, choice, manifest](bool ok2, const QByteArray& sigText, const QString& error2) {
      s->deleteLater();
      if (!ok2) return fail(tr("Could not download the signature of %1 (%2).").arg(qs(choice.tag), error2));
      const auto sig = parse_signature(sigText.toStdString());
      if (!sig) return fail(tr("The signature of %1 is not readable; the update was ignored.").arg(qs(choice.tag)));
      if (const auto why = verify(*key_, *sig, bytes_of(std::string_view(manifest.constData(), std::size_t(manifest.size()))));
          !why.empty())
        return fail(tr("The update %1 is NOT signed by the 3re release key (%2). It was ignored and nothing was downloaded.")
                        .arg(qs(choice.tag), qs(why)));
      try {
        Manifest parsed = parse_manifest(std::string_view(manifest.constData(), std::size_t(manifest.size())));
        if (!(parsed.version == choice.version)) return fail(tr("The signed description does not match release %1.").arg(qs(choice.tag)));
        if (!(*current_ < parsed.version)) return finish(false, tr("3re %1 is up to date.").arg(current_text()));
        if (parsed.channel == "alpha" && channel() != "alpha") return finish(false, tr("3re %1 is up to date (stable channel).").arg(current_text()));
        if (!parsed.package_for(kPlatform)) return fail(tr("Release %1 has no Windows package.").arg(qs(choice.tag)));
        offer_ = std::move(parsed);
        finish(true, tr("3re %1 is available.").arg(qs(offer_->version_text)));
      } catch (const std::exception& e) {
        fail(tr("The update description of %1 is not valid: %2").arg(qs(choice.tag), QString::fromUtf8(e.what())));
      }
    });
    s->get(QUrl(qs(choice.signature_url)), 64 * 1024);
  });
  m->get(QUrl(qs(choice.manifest_url)), 4 * 1024 * 1024);
}

void UpdateService::download() {
  if (busy_ || !offer_) return;
  const Package* pkg = offer_->package_for(kPlatform);
  if (!pkg) return fail(tr("No Windows package in this release."));
  if (root().isEmpty()) return fail(notUpdatableReason());
  busy_ = true;
  const QString work = workDir();
  QDir().mkpath(work);
  const QString zip = QDir(work).filePath(qs(pkg->name));
  const QString version = qs(offer_->version_text);
  download_ = new Download(nam_, this);
  connect(download_, &Download::progress, this, [this, version](qint64 done, qint64 total) {
    emit progress(done, total, tr("Downloading 3re %1").arg(version));
  });
  connect(download_, &Download::finished, this, [this, zip, work, version](bool ok, const QString& error) {
    download_->deleteLater();
    if (!ok) return fail(tr("The download stopped: %1. Nothing was changed.").arg(error));
    emit progress(0, 0, tr("Checking every file of 3re %1").arg(version));
    const Package package = *offer_->package_for(kPlatform);
    const QString stage = QDir(work).filePath("stage-" + version);
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, stage] {
      watcher->deleteLater();
      const QString error2 = watcher->result();
      if (!error2.isEmpty()) {
        QDir(stage).removeRecursively();
        return fail(tr("The downloaded package did not pass the check: %1. Nothing was changed.").arg(error2));
      }
      staged_ = stage;
      busy_ = false;
      emit ready(stage);
    });
    watcher->setFuture(QtConcurrent::run([zip, stage, package]() -> QString {
      QDir(stage).removeRecursively();
      if (QString e = unpack(zip, stage); !e.isEmpty()) return e;
      if (QString e = verifyTree(stage, package); !e.isEmpty()) return e;
      QFile::remove(zip);
      return {};
    }));
  });
  download_->start(QUrl(qs(pkg->url)), zip, qint64(pkg->size), qs(pkg->sha256));
}

void UpdateService::cancel() {
  if (download_) download_->cancel();
}

QString UpdateService::unpack(const QString& zip, const QString& dir) {
#ifdef Q_OS_WIN
  FILE* fp = _wfopen(reinterpret_cast<const wchar_t*>(zip.utf16()), L"rb");
#else
  FILE* fp = std::fopen(QFile::encodeName(zip).constData(), "rb");
#endif
  if (!fp) return tr("cannot open %1").arg(QDir::toNativeSeparators(zip));
  mz_zip_archive za{};
  if (!mz_zip_reader_init_cfile(&za, fp, mz_uint64(QFileInfo(zip).size()), 0)) {
    std::fclose(fp);
    return tr("the package is not a valid ZIP file");
  }
  QString error;
  const mz_uint n = mz_zip_reader_get_num_files(&za);
  for (mz_uint i = 0; i < n && error.isEmpty(); ++i) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&za, i, &st)) { error = tr("unreadable entry %1").arg(i); break; }
    if (mz_zip_reader_is_file_a_directory(&za, i)) continue;
    const std::string name = st.m_filename;
    if (!safe_relative_path(name)) { error = tr("unsafe path in the package: %1").arg(qs(name)); break; }
    const QString target = QDir(dir).filePath(qs(name));
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile out(target);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) { error = tr("cannot write %1").arg(QDir::toNativeSeparators(target)); break; }
    if (!mz_zip_reader_extract_to_callback(&za, i, write_to_file, &out, 0)) error = tr("cannot unpack %1").arg(qs(name));
  }
  mz_zip_reader_end(&za);
  std::fclose(fp);
  return error;
}

QString UpdateService::verifyTree(const QString& dir, const Package& package) {
  if (package.files.empty()) return tr("the signed description lists no files");
  QHash<QString, const FileEntry*> expected;
  for (const auto& f : package.files) expected.insert(qs(f.path), &f);
  for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
    qint64 size = -1;
    const QString sha = Download::sha256File(QDir(dir).filePath(it.key()), &size);
    if (sha.isEmpty()) return tr("%1 is missing").arg(it.key());
    if (size != qint64(it.value()->size) || sha != qs(it.value()->sha256)) return tr("%1 does not match the signed checksum").arg(it.key());
  }
  QDirIterator walk(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
  while (walk.hasNext()) {
    const QString rel = QDir(dir).relativeFilePath(walk.next());
    if (!expected.contains(rel)) return tr("%1 is not part of the signed release").arg(rel);
  }
  return {};
}

bool UpdateService::launch(const QStringList& args, QString* error) {
#ifdef Q_OS_WIN
  // Run a copy from the temp folder: the install folder is renamed while the updater runs.
  const QString source = QDir(QCoreApplication::applicationDirPath()).filePath("3re-update.exe");
  const QString copy = QDir::temp().filePath(QStringLiteral("3re-update-%1.exe").arg(QCoreApplication::applicationPid()));
  QFile::remove(copy);
  if (!QFile::copy(source, copy)) {
    if (error) *error = tr("3re-update.exe is missing next to 3re-studio.exe");
    return false;
  }
  std::wstring line = quote(copy);
  for (const auto& a : args) line += L" " + quote(a);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const std::wstring exe = copy.toStdWString();
  // Leave 3re.exe's kill-on-close job, or the updater would end together with 3re.
  BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi);
  if (!ok) ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!ok) {
    if (error) *error = tr("could not start the updater (Windows error %1)").arg(GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  Q_UNUSED(args);
  if (error) *error = tr("automatic installation is only available on Windows for now");
  return false;
#endif
}

bool UpdateService::startUpdater(bool relaunch, QString* error) {
  if (staged_.isEmpty()) {
    if (error) *error = tr("no downloaded update is ready");
    return false;
  }
  QStringList args{"--install", QDir::toNativeSeparators(root()), "--staged", QDir::toNativeSeparators(staged_), "--wait-pid",
                   QString::number(QCoreApplication::applicationPid()), "--result", QDir::toNativeSeparators(resultFile())};
  if (const QString launcher = qEnvironmentVariable("THREERE_LAUNCHER_PID"); !launcher.isEmpty()) args << "--wait-pid" << launcher;
  if (relaunch) args << "--relaunch";
  return launch(args, error);
}

bool UpdateService::startRollback(QString* error) {
  if (previousVersion().isEmpty()) {
    if (error) *error = tr("there is no previous version to go back to");
    return false;
  }
  QStringList args{"--rollback", "--install", QDir::toNativeSeparators(root()), "--wait-pid",
                   QString::number(QCoreApplication::applicationPid()), "--result", QDir::toNativeSeparators(resultFile()), "--relaunch"};
  if (const QString launcher = qEnvironmentVariable("THREERE_LAUNCHER_PID"); !launcher.isEmpty()) args << "--wait-pid" << launcher;
  return launch(args, error);
}

}  // namespace threere::update
