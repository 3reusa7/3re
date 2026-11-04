#pragma once
// One verified download for updates and first-run models: the file grows as "<target>.part", resumes with an
// HTTP Range request after an interruption, and only gets its final name when its size and SHA-256 match.
// A mismatch deletes the partial file. No cookies, no identifiers: the request carries a plain
// "3re-update" User-Agent and nothing else about the person or the machine.
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;

namespace threere::update {

class Download : public QObject {
  Q_OBJECT
 public:
  explicit Download(QNetworkAccessManager* nam, QObject* parent = nullptr);
  ~Download() override;

  // size < 0: unknown (checked only by SHA-256). sha256: lower-case hex.
  void start(const QUrl& url, const QString& target, qint64 size, const QString& sha256);
  void cancel();
  bool running() const { return reply_ != nullptr; }

  static QString sha256File(const QString& file, qint64* size = nullptr);

 signals:
  void progress(qint64 received, qint64 total);
  void finished(bool ok, const QString& error);

 private:
  void request();
  void onReadyRead();
  void onFinished();
  void fail(const QString& error, bool removePart);

  QNetworkAccessManager* nam_;
  QPointer<QNetworkReply> reply_;
  QUrl url_;
  QString target_, sha256_;
  qint64 size_ = -1, have_ = 0;
  QFile part_;
  QCryptographicHash hash_{QCryptographicHash::Sha256};
  bool restarted_ = false, cancelled_ = false;
};

// GET a small document (release list, manifest, signature) into memory; at most `limit` bytes.
class Fetch : public QObject {
  Q_OBJECT
 public:
  explicit Fetch(QNetworkAccessManager* nam, QObject* parent = nullptr) : QObject(parent), nam_(nam) {}
  void get(const QUrl& url, qint64 limit = 4 * 1024 * 1024, const QByteArray& accept = {});
 signals:
  void done(bool ok, const QByteArray& body, const QString& error);

 private:
  QNetworkAccessManager* nam_;
};

QNetworkRequest plain_request(const QUrl& url);

}  // namespace threere::update
