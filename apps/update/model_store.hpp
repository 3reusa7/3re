#pragma once
// First-run downloads (app/models.json, written by tools/release/make_models_catalog.py): the AI weights and
// the GPU engine libraries are not part of the 3re package. Each pack names its size, source and licence
// before anything is downloaded; every file is checked by size and SHA-256 before it is used; archives
// are unpacked and every extracted file is checked too. Files land at their final path only when they
// match (a partial download stays as ".part" and resumes next time).
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QNetworkAccessManager;

namespace threere::update {

class Download;

struct ModelFile {
  QString url, member, path, sha256;  // url for plain files, member for files inside the pack's archive
  qint64 size = 0;
};

struct ModelPack {
  QString id, title, purpose, source, licence, attribution;
  QStringList licence_urls;
  QString archive_name, archive_url, archive_sha256;
  qint64 archive_size = 0;
  QList<ModelFile> files;

  bool archived() const { return !archive_url.isEmpty(); }
  qint64 downloadSize() const;
};

enum class PackState { Ready, Missing, Partial, Damaged };

class ModelStore : public QObject {
  Q_OBJECT
 public:
  explicit ModelStore(QObject* parent = nullptr);

  // <app>/models.json of a release; development builds fall back to the copy in the source tree.
  static QString defaultCatalog();
  // The folder the catalog paths are relative to: the 3re release folder (development builds: none).
  static QString defaultRoot();
  bool load(const QString& catalog, const QString& root, QString* error = nullptr);

  const QList<ModelPack>& packs() const { return packs_; }
  QString root() const { return root_; }
  // deep: also re-check every SHA-256 (seconds per GB); otherwise sizes only.
  PackState state(const ModelPack& pack, bool deep = false, QString* detail = nullptr) const;
  QList<ModelPack> missing() const;  // not Ready (by size)
  static QString sizeText(qint64 bytes);

  void download(const QStringList& ids);
  void cancel();
  bool busy() const { return busy_; }

 signals:
  void progress(qint64 done, qint64 total, const QString& what);
  void finished(bool ok, const QString& message);

 private:
  void next();
  void fail(const QString& why);
  QString unpackPack(const ModelPack& pack, const QString& archive) const;

  QNetworkAccessManager* nam_;
  QString root_;
  QList<ModelPack> packs_;
  // the queue: one step per download (a plain file, or a pack's archive)
  struct Step {
    int pack = -1, file = -1;  // file -1: the archive
  };
  QList<Step> queue_;
  qint64 total_ = 0, before_ = 0;
  QPointer<Download> current_;
  bool busy_ = false, cancelled_ = false;
};

}  // namespace threere::update
