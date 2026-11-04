#include "update/model_store.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QtConcurrent/QtConcurrentRun>

#include <cstdio>

#include "miniz.h"
#include "update/download.hpp"
#include "update/update_service.hpp"

namespace threere::update {
namespace {

struct Sink {
  QFile* file;
  QCryptographicHash* hash;
};

size_t write_hashed(void* opaque, mz_uint64, const void* buf, size_t n) {
  auto* s = static_cast<Sink*>(opaque);
  s->hash->addData(QByteArrayView(static_cast<const char*>(buf), qsizetype(n)));
  return s->file->write(static_cast<const char*>(buf), qint64(n)) == qint64(n) ? n : 0;
}

}  // namespace

qint64 ModelPack::downloadSize() const {
  if (archived()) return archive_size;
  qint64 s = 0;
  for (const auto& f : files) s += f.size;
  return s;
}

ModelStore::ModelStore(QObject* parent) : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

QString ModelStore::defaultCatalog() {
  const QString shipped = QDir(QCoreApplication::applicationDirPath()).filePath("models.json");
  if (QFileInfo(shipped).isFile()) return shipped;
#ifdef THREERE_SOURCE_MODELS_CATALOG
  return QStringLiteral(THREERE_SOURCE_MODELS_CATALOG);
#else
  return shipped;
#endif
}

QString ModelStore::defaultRoot() { return UpdateService::installRoot(); }

bool ModelStore::load(const QString& catalog, const QString& root, QString* error) {
  packs_.clear();
  root_ = root;
  QFile f(catalog);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = tr("%1 is missing").arg(QDir::toNativeSeparators(catalog));
    return false;
  }
  const auto doc = QJsonDocument::fromJson(f.readAll()).object();
  if (doc.value("format").toString() != "3re-models" || doc.value("format_version").toInt() != 1) {
    if (error) *error = tr("%1 is not a 3re model list").arg(QDir::toNativeSeparators(catalog));
    return false;
  }
  for (const auto& pv : doc.value("packs").toArray()) {
    const auto p = pv.toObject();
    ModelPack pack;
    pack.id = p.value("id").toString();
    pack.title = p.value("title").toString();
    pack.purpose = p.value("purpose").toString();
    pack.source = p.value("source").toString();
    pack.licence = p.value("licence").toString();
    pack.attribution = p.value("attribution").toString();
    for (const auto& u : p.value("licence_urls").toArray()) pack.licence_urls << u.toString();
    const auto a = p.value("archive").toObject();
    pack.archive_name = a.value("name").toString();
    pack.archive_url = a.value("url").toString();
    pack.archive_sha256 = a.value("sha256").toString();
    pack.archive_size = qint64(a.value("size").toDouble());
    for (const auto& fv : p.value("files").toArray()) {
      const auto o = fv.toObject();
      ModelFile file{o.value("url").toString(), o.value("member").toString(), o.value("path").toString(), o.value("sha256").toString(),
                     qint64(o.value("size").toDouble())};
      const bool safe = safe_relative_path(file.path.toStdString());
      if (!safe || file.sha256.size() != 64 || file.size <= 0 || (file.url.isEmpty() && file.member.isEmpty())) {
        if (error) *error = tr("model list entry %1 is not valid").arg(file.path);
        packs_.clear();
        return false;
      }
      pack.files << file;
    }
    packs_ << pack;
  }
  return true;
}

PackState ModelStore::state(const ModelPack& pack, bool deep, QString* detail) const {
  int missing = 0, wrong = 0;
  QStringList notes;
  for (const auto& f : pack.files) {
    const QString path = QDir(root_).filePath(f.path);
    const QFileInfo info(path);
    if (!info.isFile()) {
      ++missing;
      continue;
    }
    if (info.size() != f.size) {
      ++wrong;
      notes << tr("%1 has the wrong size").arg(info.fileName());
    } else if (deep && Download::sha256File(path) != f.sha256) {
      ++wrong;
      notes << tr("%1 does not match its checksum").arg(info.fileName());
    }
  }
  if (detail) *detail = notes.join("; ");
  if (wrong) return PackState::Damaged;
  if (missing == pack.files.size()) return PackState::Missing;
  return missing ? PackState::Partial : PackState::Ready;
}

QList<ModelPack> ModelStore::missing() const {
  QList<ModelPack> out;
  for (const auto& p : packs_)
    if (state(p) != PackState::Ready) out << p;
  return out;
}

QString ModelStore::sizeText(qint64 bytes) {
  if (bytes >= 1000LL * 1000 * 1000) return QStringLiteral("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 2);
  return QStringLiteral("%1 MB").arg(double(bytes) / 1e6, 0, 'f', 0);
}

void ModelStore::download(const QStringList& ids) {
  if (busy_ || root_.isEmpty()) return;
  queue_.clear();
  total_ = before_ = 0;
  for (int i = 0; i < packs_.size(); ++i) {
    const auto& p = packs_[i];
    if (!ids.contains(p.id)) continue;
    if (p.archived()) {
      if (state(p) != PackState::Ready) {
        queue_ << Step{i, -1};
        total_ += p.archive_size;
      }
      continue;
    }
    for (int j = 0; j < p.files.size(); ++j) {
      const QFileInfo info(QDir(root_).filePath(p.files[j].path));
      if (info.isFile() && info.size() == p.files[j].size) continue;
      queue_ << Step{i, j};
      total_ += p.files[j].size;
    }
  }
  busy_ = true;
  cancelled_ = false;
  next();
}

void ModelStore::cancel() {
  cancelled_ = true;
  if (current_) current_->cancel();
}

void ModelStore::fail(const QString& why) {
  busy_ = false;
  queue_.clear();
  emit finished(false, why);
}

void ModelStore::next() {
  if (cancelled_) return fail(tr("Cancelled. What was downloaded so far is kept and the download resumes next time."));
  if (queue_.isEmpty()) {
    busy_ = false;
    emit finished(true, tr("All models are downloaded and checked."));
    return;
  }
  const Step step = queue_.takeFirst();
  const ModelPack& pack = packs_[step.pack];
  const bool archive = step.file < 0;
  const QString url = archive ? pack.archive_url : pack.files[step.file].url;
  const QString target = archive ? QDir(root_).filePath("models/.downloads/" + pack.archive_name) : QDir(root_).filePath(pack.files[step.file].path);
  const qint64 size = archive ? pack.archive_size : pack.files[step.file].size;
  const QString sha = archive ? pack.archive_sha256 : pack.files[step.file].sha256;
  const QString what = archive ? pack.title : QFileInfo(target).fileName();
  current_ = new Download(nam_, this);
  connect(current_, &Download::progress, this, [this, what](qint64 got, qint64) { emit progress(before_ + got, total_, what); });
  connect(current_, &Download::finished, this, [this, step, archive, target, size](bool ok, const QString& error) {
    current_->deleteLater();
    if (!ok) return fail(tr("Download failed: %1").arg(error));
    before_ += size;
    if (!archive) return next();
    emit progress(before_, total_, tr("Unpacking %1").arg(packs_[step.pack].title));
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, target] {
      watcher->deleteLater();
      const QString error2 = watcher->result();
      if (!error2.isEmpty()) return fail(error2);
      QFile::remove(target);
      next();
    });
    const ModelPack pack = packs_[step.pack];
    watcher->setFuture(QtConcurrent::run([this, pack, target] { return unpackPack(pack, target); }));
  });
  current_->start(QUrl(url), target, size, sha);
}

QString ModelStore::unpackPack(const ModelPack& pack, const QString& archive) const {
#ifdef Q_OS_WIN
  FILE* fp = _wfopen(reinterpret_cast<const wchar_t*>(archive.utf16()), L"rb");
#else
  FILE* fp = std::fopen(QFile::encodeName(archive).constData(), "rb");
#endif
  if (!fp) return tr("cannot open %1").arg(QDir::toNativeSeparators(archive));
  mz_zip_archive za{};
  if (!mz_zip_reader_init_cfile(&za, fp, mz_uint64(QFileInfo(archive).size()), 0)) {
    std::fclose(fp);
    return tr("%1 is not a valid ZIP file").arg(pack.archive_name);
  }
  QString error;
  for (const auto& f : pack.files) {
    const int index = mz_zip_reader_locate_file(&za, f.member.toUtf8().constData(), nullptr, 0);
    mz_zip_archive_file_stat st;
    if (index < 0 || !mz_zip_reader_file_stat(&za, mz_uint(index), &st)) {
      error = tr("%1 is not in %2").arg(f.member, pack.archive_name);
      break;
    }
    if (qint64(st.m_uncomp_size) != f.size) {
      error = tr("%1 in %2 has the wrong size").arg(f.member, pack.archive_name);
      break;
    }
    const QString target = QDir(root_).filePath(f.path);
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile out(target + ".part");
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      error = tr("cannot write %1").arg(QDir::toNativeSeparators(out.fileName()));
      break;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    Sink sink{&out, &hash};
    const bool ok = mz_zip_reader_extract_to_callback(&za, mz_uint(index), write_hashed, &sink, 0);
    out.close();
    if (!ok || QString::fromLatin1(hash.result().toHex()) != f.sha256) {
      out.remove();
      error = tr("%1 does not match its checksum").arg(f.member);
      break;
    }
    QFile::remove(target);
    if (!out.rename(target)) {
      error = tr("cannot rename %1").arg(QDir::toNativeSeparators(target));
      break;
    }
  }
  mz_zip_reader_end(&za);
  std::fclose(fp);
  return error;
}

}  // namespace threere::update
