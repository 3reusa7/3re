#include "update/download.hpp"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

namespace threere::update {

QNetworkRequest plain_request(const QUrl& url) {
  QNetworkRequest r(url);
  r.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("3re-update"));
  r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  r.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  r.setTransferTimeout(60000);
  return r;
}

void Fetch::get(const QUrl& url, qint64 limit, const QByteArray& accept) {
  auto req = plain_request(url);
  if (!accept.isEmpty()) req.setRawHeader("Accept", accept);
  QNetworkReply* reply = nam_->get(req);
  connect(reply, &QNetworkReply::downloadProgress, this, [reply, limit](qint64 got, qint64) {
    if (got > limit) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, limit] {
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
      emit done(false, {}, reply->error() == QNetworkReply::OperationCanceledError ? tr("the answer was too large")
                                                                                     : reply->errorString());
      return;
    }
    if (status && status != 200) {
      emit done(false, {}, tr("the server answered HTTP %1").arg(status));
      return;
    }
    const QByteArray body = reply->readAll();
    if (body.size() > limit) {
      emit done(false, {}, tr("the answer was too large"));
      return;
    }
    emit done(true, body, {});
  });
}

Download::Download(QNetworkAccessManager* nam, QObject* parent) : QObject(parent), nam_(nam) {}

Download::~Download() {
  if (reply_) reply_->abort();
}

QString Download::sha256File(const QString& file, qint64* size) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return {};
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData(&f);
  if (size) *size = f.size();
  return QString::fromLatin1(h.result().toHex());
}

void Download::start(const QUrl& url, const QString& target, qint64 size, const QString& sha256) {
  url_ = url;
  target_ = target;
  size_ = size;
  sha256_ = sha256.toLower();
  cancelled_ = false;
  restarted_ = false;
  QDir().mkpath(QFileInfo(target).absolutePath());
  part_.setFileName(target + ".part");
  hash_.reset();
  have_ = 0;
  if (part_.exists()) {
    // Resume: hash what is already there, then ask for the rest. A part longer than the file is garbage.
    if (size_ >= 0 && part_.size() > size_) part_.remove();
    else if (part_.open(QIODevice::ReadOnly)) {
      hash_.addData(&part_);
      have_ = part_.size();
      part_.close();
    }
  }
  // Results are always reported from the event loop, never from inside start().
  if (!part_.open(QIODevice::WriteOnly | QIODevice::Append)) {
    const QString why = tr("cannot write %1: %2").arg(QDir::toNativeSeparators(part_.fileName()), part_.errorString());
    QTimer::singleShot(0, this, [this, why] { fail(why, false); });
    return;
  }
  if (size_ >= 0 && have_ == size_) {
    QTimer::singleShot(0, this, &Download::onFinished);  // already complete: just verify
    return;
  }
  request();
}

void Download::request() {
  auto req = plain_request(url_);
  if (have_ > 0) req.setRawHeader("Range", "bytes=" + QByteArray::number(have_) + "-");
  reply_ = nam_->get(req);
  connect(reply_, &QNetworkReply::metaDataChanged, this, [this] {
    if (!reply_) return;
    const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (have_ > 0 && status == 200 && !restarted_) {
      // The server ignored the Range request: start over from the first byte.
      restarted_ = true;
      part_.resize(0);
      part_.seek(0);
      hash_.reset();
      have_ = 0;
    }
  });
  connect(reply_, &QNetworkReply::readyRead, this, &Download::onReadyRead);
  connect(reply_, &QNetworkReply::finished, this, &Download::onFinished);
}

void Download::onReadyRead() {
  if (!reply_) return;
  const QByteArray chunk = reply_->readAll();
  if (chunk.isEmpty()) return;
  if (part_.write(chunk) != chunk.size()) {
    fail(tr("cannot write %1 (disk full?)").arg(QDir::toNativeSeparators(part_.fileName())), false);
    return;
  }
  hash_.addData(chunk);
  have_ += chunk.size();
  if (size_ >= 0 && have_ > size_) {
    fail(tr("the download is larger than expected"), true);
    return;
  }
  emit progress(have_, size_);
}

void Download::onFinished() {
  QNetworkReply* reply = reply_;
  reply_ = nullptr;
  if (reply) {
    disconnect(reply, nullptr, this, nullptr);
    if (reply->bytesAvailable()) {
      reply_ = reply;
      onReadyRead();
      reply_ = nullptr;
    }
    reply->deleteLater();
    if (cancelled_) {
      part_.close();
      emit finished(false, tr("cancelled"));
      return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
      part_.close();  // keep the part: the next start resumes it
      emit finished(false, reply->errorString());
      return;
    }
    if (status && status != 200 && status != 206) {
      part_.close();
      emit finished(false, tr("the server answered HTTP %1").arg(status));
      return;
    }
  }
  part_.close();
  if (size_ >= 0 && have_ != size_) {
    emit finished(false, tr("the download stopped early (%1 of %2 bytes); try again to resume").arg(have_).arg(size_));
    return;
  }
  const QString got = QString::fromLatin1(hash_.result().toHex());
  if (!sha256_.isEmpty() && got != sha256_) {
    fail(tr("the downloaded file is damaged or not the expected one (SHA-256 %1, expected %2); it was deleted")
             .arg(got.left(12), sha256_.left(12)),
         true);
    return;
  }
  QFile::remove(target_);
  if (!QFile::rename(part_.fileName(), target_)) {
    fail(tr("cannot rename %1").arg(QDir::toNativeSeparators(part_.fileName())), false);
    return;
  }
  emit finished(true, {});
}

void Download::cancel() {
  cancelled_ = true;
  if (reply_) reply_->abort();
}

void Download::fail(const QString& error, bool removePart) {
  if (reply_) {
    QNetworkReply* r = reply_;
    reply_ = nullptr;
    disconnect(r, nullptr, this, nullptr);
    r->abort();
    r->deleteLater();
  }
  part_.close();
  if (removePart) part_.remove();
  emit finished(false, error);
}

}  // namespace threere::update
