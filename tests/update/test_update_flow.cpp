// End-to-end update and first-run download tests against a local HTTP server that plays GitHub Releases
// and Hugging Face: an update from the previous release (check -> signed manifest -> download -> verify ->
// 3re-update.exe swap -> self-test), a bad signature, a corrupted download, a release that fails its
// self-test (rolled back), and the model store's verified downloads (plain file and archive).
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include "miniz.h"
#include "update/download.hpp"
#include "update/model_store.hpp"
#include "update/update_service.hpp"

using namespace threere::update;

namespace {

// A tiny HTTP/1.1 server: GET <path> -> the registered body (Range requests are answered with 206).
class Server : public QObject {
 public:
  QHash<QString, QByteArray> files;
  QStringList requests;
  Server() {
    QObject::connect(&tcp_, &QTcpServer::newConnection, this, [this] {
      while (auto* s = tcp_.nextPendingConnection()) {
        QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
          const QByteArray req = s->readAll();
          const QList<QByteArray> lines = req.split('\n');
          const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
          const QString path = QString::fromLatin1(first.value(1));
          requests << path;
          qint64 from = 0;
          for (const auto& l : lines)
            if (l.toLower().startsWith("range: bytes=")) from = l.trimmed().mid(13).split('-').value(0).toLongLong();
          if (!files.contains(path)) {
            s->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          } else {
            const QByteArray body = files.value(path).mid(from);
            s->write((from ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") + QByteArray("Content-Length: ") +
                     QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
          }
          s->disconnectFromHost();
        });
        QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
      }
    });
    tcp_.listen(QHostAddress::LocalHost, 0);
  }
  QString url(const QString& path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(tcp_.serverPort()).arg(path); }

 private:
  QTcpServer tcp_;
};

QByteArray readAll(const QString& f) {
  QFile file(f);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void writeAll(const QString& f, const QByteArray& data) {
  QDir().mkpath(QFileInfo(f).absolutePath());
  QFile file(f);
  QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(data);
}

QString sha(const QByteArray& b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); }

QByteArray zip(const QMap<QString, QByteArray>& files) {
  mz_zip_archive za{};
  mz_zip_writer_init_heap(&za, 0, 0);
  for (auto it = files.cbegin(); it != files.cend(); ++it)
    mz_zip_writer_add_mem(&za, it.key().toUtf8().constData(), it.value().constData(), size_t(it.value().size()), MZ_DEFAULT_COMPRESSION);
  void* buf = nullptr;
  size_t size = 0;
  mz_zip_writer_finalize_heap_archive(&za, &buf, &size);
  QByteArray out(static_cast<const char*>(buf), qsizetype(size));
  mz_zip_writer_end(&za);
  return out;
}

// A release tree made of the fake app (3re.exe), plus its RELEASE.json.
QMap<QString, QByteArray> releaseTree(const QString& version, bool selftestPasses) {
  QMap<QString, QByteArray> t;
  t["3re.exe"] = readAll(QStringLiteral(FAKE_RELEASE_APP));
  t["selftest.txt"] = selftestPasses ? "pass" : "fail";
  t["app/version.txt"] = version.toUtf8();
  QJsonArray list;
  for (auto it = t.cbegin(); it != t.cend(); ++it) list.append(QJsonObject{{"path", it.key()}, {"size", it.value().size()}, {"sha256", sha(it.value())}});
  t["RELEASE.json"] = QJsonDocument(QJsonObject{{"product", "3re"}, {"version", version}, {"files", list}}).toJson();
  return t;
}

const Seed kSeed = [] {
  Seed s;
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = std::uint8_t(0x30 + i);
  return s;
}();
const KeyId kId{9, 9, 9, 9, 1, 2, 3, 4};

QByteArray signText(const QByteArray& data, const Seed& seed = kSeed) {
  return QByteArray::fromStdString(sign(seed, kId, bytes_of(std::string_view(data.constData(), std::size_t(data.size()))), "3re test manifest"));
}

}  // namespace

class UpdateFlowTests : public QObject {
  Q_OBJECT
  Server server_;
  QTemporaryDir tmp_;

  // Publishes release `version` on the server; returns the package bytes as published.
  QByteArray publish(const QString& version, bool selftestPasses, bool tamperManifest = false, bool corruptPackage = false,
                     const Seed& seed = kSeed) {
    const auto tree = releaseTree(version, selftestPasses);
    QByteArray package = zip(tree);
    QJsonArray files;
    for (auto it = tree.cbegin(); it != tree.cend(); ++it) files.append(QJsonObject{{"path", it.key()}, {"size", it.value().size()}, {"sha256", sha(it.value())}});
    const QJsonObject pkg{{"platform", "windows-x64"}, {"kind", "portable-zip"}, {"name", "3re-" + version + "-windows-x64.zip"},
                          {"url", server_.url("/package.zip")}, {"size", package.size()}, {"sha256", sha(package)}, {"files", files}};
    QByteArray manifest = QJsonDocument(QJsonObject{{"format", "3re-update-manifest"}, {"format_version", 1}, {"product", "3re"},
                                                    {"version", version}, {"channel", "alpha"},
                                                    {"notes", QJsonObject{{"en", "Plain words."}, {"ar", "كلمات بسيطة."}}},
                                                    {"packages", QJsonArray{pkg}}})
                              .toJson();
    const QByteArray signature = signText(manifest, seed);
    if (tamperManifest) manifest.replace("Plain words.", "Plain words!");
    if (corruptPackage) package[package.size() / 2] = char(package[package.size() / 2] ^ 0x5a);
    server_.files["/package.zip"] = package;
    server_.files["/manifest.json"] = manifest;
    server_.files["/manifest.json.minisig"] = signature;
    const QJsonArray releases{QJsonObject{{"tag_name", "v" + version}, {"prerelease", true}, {"draft", false}, {"html_url", "https://example.invalid"},
                                          {"assets", QJsonArray{QJsonObject{{"name", "update-manifest.json"}, {"browser_download_url", server_.url("/manifest.json")}},
                                                                QJsonObject{{"name", "update-manifest.json.minisig"}, {"browser_download_url", server_.url("/manifest.json.minisig")}}}}}};
    server_.files["/releases"] = QJsonDocument(releases).toJson();
    return package;
  }

  QString install(const QString& name, const QString& version) {
    const QString root = tmp_.filePath(name + "/3re");
    QDir(tmp_.filePath(name)).removeRecursively();
    const auto tree = releaseTree(version, true);
    for (auto it = tree.cbegin(); it != tree.cend(); ++it) writeAll(QDir(root).filePath(it.key()), it.value());
    writeAll(QDir(root).filePath("models/trellis/q4/dinov3.gguf"), "the person's downloaded weights");
    return root;
  }

  void configure(UpdateService& s, const QString& root) {
    s.setFeedUrl(QUrl(server_.url("/releases")));
    s.setTrustedKey(public_key_from_seed(kSeed, kId));
    s.setCurrentVersion("2.9.1-alpha");
    s.setInstallRoot(root);
  }

  QJsonObject runUpdater(const QString& root, const QString& staged) {
    const QString result = tmp_.filePath("result.json");
    QFile::remove(result);
    QProcess p;
    p.start(QStringLiteral(UPDATER_EXE), {"--install", QDir::toNativeSeparators(root), "--staged", QDir::toNativeSeparators(staged), "--result", result});
    p.waitForFinished(120000);
    return QJsonDocument::fromJson(readAll(result)).object();
  }

 private slots:
  void initTestCase() {
    QCoreApplication::setOrganizationName("3re-tests");
    QCoreApplication::setApplicationName("update-flow");
    QSettings().clear();
    UpdateService::setChannel("alpha");
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    QVERIFY(tmp_.isValid());
  }

  void updateFromPreviousRelease() {
    publish("2.10.0-alpha", true);
    const QString root = install("ok", "2.9.1-alpha");
    UpdateService s;
    configure(s, root);
    QSignalSpy checked(&s, &UpdateService::checked), ready(&s, &UpdateService::ready), failed(&s, &UpdateService::failed);
    s.check();
    QTRY_COMPARE_WITH_TIMEOUT(checked.count() + failed.count(), 1, 10000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    QCOMPARE(checked.first().first().toBool(), true);
    QVERIFY(s.offer());
    QCOMPARE(QString::fromStdString(s.offer()->notes_ar), QStringLiteral("كلمات بسيطة."));
    s.download();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count() + failed.count(), 1, 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    const QString staged = ready.first().first().toString();
    QCOMPARE(readAll(QDir(staged).filePath("app/version.txt")), QByteArray("2.10.0-alpha"));
    const auto out = runUpdater(root, staged);
    QCOMPARE(out.value("status").toString(), QStringLiteral("updated"));
    QCOMPARE(readAll(QDir(root).filePath("app/version.txt")), QByteArray("2.10.0-alpha"));
    QCOMPARE(readAll(QDir(root).filePath("models/trellis/q4/dinov3.gguf")), QByteArray("the person's downloaded weights"));
    QCOMPARE(readAll(QDir(root + ".previous").filePath("app/version.txt")), QByteArray("2.9.1-alpha"));
  }

  void badSignatureIsRefused() {
    publish("2.10.0-alpha", true, /*tamperManifest=*/true);
    const QString root = install("badsig", "2.9.1-alpha");
    UpdateService s;
    configure(s, root);
    QSignalSpy checked(&s, &UpdateService::checked), failed(&s, &UpdateService::failed);
    server_.requests.clear();
    s.check();
    QTRY_COMPARE_WITH_TIMEOUT(checked.count() + failed.count(), 1, 10000);
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().contains("NOT signed"));
    QVERIFY(!s.offer());
    QVERIFY(!server_.requests.contains("/package.zip"));  // nothing was downloaded
  }

  void otherKeyIsRefused() {
    Seed other = kSeed;
    other[0] ^= 1;
    publish("2.10.0-alpha", true, false, false, other);
    const QString root = install("otherkey", "2.9.1-alpha");
    UpdateService s;
    configure(s, root);
    QSignalSpy failed(&s, &UpdateService::failed);
    s.check();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
    QVERIFY(failed.first().first().toString().contains("NOT signed"));
  }

  void corruptedDownloadChangesNothing() {
    publish("2.10.0-alpha", true, false, /*corruptPackage=*/true);
    const QString root = install("corrupt", "2.9.1-alpha");
    UpdateService s;
    configure(s, root);
    QSignalSpy checked(&s, &UpdateService::checked), ready(&s, &UpdateService::ready), failed(&s, &UpdateService::failed);
    s.check();
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 10000);
    s.download();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count() + failed.count(), 1, 20000);
    QCOMPARE(ready.count(), 0);
    QVERIFY(failed.first().first().toString().contains("SHA-256"));
    const QString work = QDir::cleanPath(root + "/../.3re-update");
    QVERIFY(!QFileInfo::exists(QDir(work).filePath("stage-2.10.0-alpha")));
    QVERIFY(QDir(work).entryList({"*.part", "*.zip"}, QDir::Files).isEmpty());
    QCOMPARE(readAll(QDir(root).filePath("app/version.txt")), QByteArray("2.9.1-alpha"));
  }

  void failingReleaseIsRolledBack() {
    publish("2.10.0-alpha", /*selftestPasses=*/false);
    const QString root = install("rollback", "2.9.1-alpha");
    UpdateService s;
    configure(s, root);
    QSignalSpy checked(&s, &UpdateService::checked), ready(&s, &UpdateService::ready), failed(&s, &UpdateService::failed);
    s.check();
    QTRY_COMPARE_WITH_TIMEOUT(checked.count(), 1, 10000);
    s.download();
    QTRY_COMPARE_WITH_TIMEOUT(ready.count() + failed.count(), 1, 20000);
    QCOMPARE(ready.count(), 1);
    const auto out = runUpdater(root, ready.first().first().toString());
    QCOMPARE(out.value("status").toString(), QStringLiteral("rolled_back"));
    QCOMPARE(readAll(QDir(root).filePath("app/version.txt")), QByteArray("2.9.1-alpha"));
    QCOMPARE(readAll(QDir(root).filePath("models/trellis/q4/dinov3.gguf")), QByteArray("the person's downloaded weights"));
  }

  void resumedDownloadIsVerified() {
    const QByteArray data = QByteArray(300000, 'm') + "end";
    server_.files["/weights.bin"] = data;
    const QString target = tmp_.filePath("resume/weights.bin");
    writeAll(target + ".part", data.left(120000));  // an interrupted earlier download
    QNetworkAccessManager nam;
    Download d(&nam);
    QSignalSpy done(&d, &Download::finished);
    server_.requests.clear();
    d.start(QUrl(server_.url("/weights.bin")), target, data.size(), sha(data));
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 10000);
    QVERIFY2(done.first().first().toBool(), qPrintable(done.first().at(1).toString()));
    QCOMPARE(readAll(target), data);
    QVERIFY(!QFileInfo::exists(target + ".part"));
  }

  void modelStoreDownloadsAndChecks() {
    const QByteArray weights = QByteArray(50000, 'w');
    const QByteArray dll = "engine library", yaml = "config: tiny";
    server_.files["/hf/dinov3.gguf"] = weights;
    const QByteArray archive = zip({{"ggml.dll", dll}, {"config.yaml", yaml}, {"extra.txt", "not extracted"}});
    server_.files["/gh/engine.zip"] = archive;
    const QJsonObject catalog{
        {"format", "3re-models"}, {"format_version", 1},
        {"packs", QJsonArray{QJsonObject{{"id", "weights"}, {"title", "Weights"}, {"licence", "MIT"},
                                         {"files", QJsonArray{QJsonObject{{"url", server_.url("/hf/dinov3.gguf")}, {"path", "models/trellis/q4/dinov3.gguf"},
                                                                          {"size", weights.size()}, {"sha256", sha(weights)}}}}},
                             QJsonObject{{"id", "engine"}, {"title", "Engine"}, {"licence", "MIT"},
                                         {"archive", QJsonObject{{"name", "engine.zip"}, {"url", server_.url("/gh/engine.zip")}, {"size", archive.size()}, {"sha256", sha(archive)}}},
                                         {"files", QJsonArray{QJsonObject{{"member", "ggml.dll"}, {"path", "app/runtime/ggml.dll"}, {"size", dll.size()}, {"sha256", sha(dll)}},
                                                              QJsonObject{{"member", "config.yaml"}, {"path", "models/x/config.yaml"}, {"size", yaml.size()}, {"sha256", sha(yaml)}}}}}}}};
    const QString file = tmp_.filePath("models.json");
    writeAll(file, QJsonDocument(catalog).toJson());
    const QString root = tmp_.filePath("models-root");
    QDir(root).removeRecursively();
    ModelStore store;
    QString error;
    QVERIFY2(store.load(file, root, &error), qPrintable(error));
    QCOMPARE(store.missing().size(), 2);
    QSignalSpy done(&store, &ModelStore::finished);
    store.download({"weights", "engine"});
    QTRY_COMPARE_WITH_TIMEOUT(done.count(), 1, 20000);
    QVERIFY2(done.first().first().toBool(), qPrintable(done.first().at(1).toString()));
    for (const auto& p : store.packs()) QCOMPARE(int(store.state(p, true)), int(PackState::Ready));
    QCOMPARE(readAll(QDir(root).filePath("app/runtime/ggml.dll")), dll);
    QVERIFY(!QFileInfo::exists(QDir(root).filePath("models/.downloads/engine.zip")));
    QVERIFY(!QFileInfo::exists(QDir(root).filePath("extra.txt")));

    // A damaged copy on the server is refused and leaves no file behind.
    QFile::remove(QDir(root).filePath("models/trellis/q4/dinov3.gguf"));
    server_.files["/hf/dinov3.gguf"][100] = 'X';
    QSignalSpy done2(&store, &ModelStore::finished);
    store.download({"weights"});
    QTRY_COMPARE_WITH_TIMEOUT(done2.count(), 1, 20000);
    QCOMPARE(done2.first().first().toBool(), false);
    QVERIFY(!QFileInfo::exists(QDir(root).filePath("models/trellis/q4/dinov3.gguf")));
    QCOMPARE(int(store.state(store.packs().first())), int(PackState::Missing));
  }
};

QTEST_MAIN(UpdateFlowTests)
#include "test_update_flow.moc"
