#include <QDir>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "project/project_store.hpp"
#include "registry.hpp"

namespace {
QString make_image(const QString& file, QColor c = Qt::red) {
  QImage img(64, 48, QImage::Format_ARGB32);
  img.fill(c);
  img.save(file);
  return file;
}
}  // namespace

class ProjectStoreTests : public QObject {
  Q_OBJECT
 private slots:
  void project_roundtrip() {
    QTemporaryDir dir;
    const QString src = make_image(dir.filePath("knight.png"));
    auto p = p3dgui::ThreeReProject::create(dir.filePath("Projects"), "Knight", src);
    QVERIFY(QFileInfo(p.projectFile()).isFile());
    QCOMPARE(p.name, QString("Knight"));
    QCOMPARE(p.source_size, QSize(64, 48));
    QVERIFY(QFileInfo(p.sourcePath()).isFile());
    QVERIFY(p.sourcePath().startsWith(p.dir));
    QCOMPARE(p.source_sha256.size(), 64);
    p.runs << "runs/Trellis-1";
    p.result_glb = "runs/Trellis-1/geometry.glb";
    p.save();
    const auto q = p3dgui::ThreeReProject::open(p.projectFile());
    QCOMPARE(q.name, p.name);
    QCOMPARE(q.source_sha256, p.source_sha256);
    QCOMPARE(q.runs, p.runs);
    QCOMPARE(q.result_glb, p.result_glb);
    QVERIFY(q.sourceIntact());
    QFile::remove(src);  // the project keeps its own copy
    QVERIFY(p3dgui::ThreeReProject::open(p.projectFile()).sourceIntact());
  }
  void project_views_roundtrip_and_backward_compatible() {
    QTemporaryDir dir;
    auto p = p3dgui::ThreeReProject::create(dir.filePath("Projects"), "Turn", make_image(dir.filePath("front.png")));
    QVERIFY(p3dgui::ThreeReProject::open(p.projectFile()).views.isEmpty());  // an older project: no views key
    const QString back = make_image(dir.filePath("back.png"), Qt::blue), side = make_image(dir.filePath("back (2).png"), Qt::green);
    const QString rel = p.importView(back), rel2 = p.importView(side);
    QCOMPARE(rel, QString("source/angles/back.png"));
    QVERIFY(rel2 != rel && QFileInfo(p.dir + "/" + rel2).isFile());
    QCOMPARE(p.importView(p.dir + "/" + rel), rel);  // already inside: no second copy
    p.views = {{rel, "back", p3dgui::file_sha256(p.dir + "/" + rel)}, {rel2, "custom:95", {}}};
    p.save();
    const auto q = p3dgui::ThreeReProject::open(p.projectFile());
    QCOMPARE(q.views.size(), 2);
    QCOMPARE(q.views[0].file, rel);
    QCOMPARE(q.views[0].label, QString("back"));
    QCOMPARE(q.views[1].label, QString("custom:95"));
    QVERIFY(QFileInfo(q.viewPath(q.views[0])).isFile());
    QFile::remove(back);  // the project keeps its own copies
    QVERIFY(QFileInfo(p3dgui::ThreeReProject::open(p.projectFile()).viewPath(q.views[0])).isFile());
  }
  void project_names_never_collide() {
    QTemporaryDir dir;
    const QString src = make_image(dir.filePath("a.png"));
    const auto a = p3dgui::ThreeReProject::create(dir.filePath("P"), "Same", src);
    const auto b = p3dgui::ThreeReProject::create(dir.filePath("P"), "Same", src);
    QVERIFY(a.dir != b.dir);
  }
  void project_source_hash_detects_change() {
    QTemporaryDir dir;
    auto p = p3dgui::ThreeReProject::create(dir.filePath("P"), "X", make_image(dir.filePath("x.png")));
    make_image(p.sourcePath(), Qt::blue);
    QVERIFY(!p3dgui::ThreeReProject::open(p.projectFile()).sourceIntact());
  }
  void project_open_rejects_foreign_json() {
    QTemporaryDir dir;
    QFile f(dir.filePath("project.3re"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"format":"something-else"})");
    f.close();
    bool threw = false;
    try {
      p3dgui::ThreeReProject::open(f.fileName());
    } catch (const std::exception& e) {
      threw = QString::fromUtf8(e.what()).contains("not a 3re project");
    }
    QVERIFY(threw);
  }
  void project_save_is_atomic() {
    QTemporaryDir dir;
    auto p = p3dgui::ThreeReProject::create(dir.filePath("P"), "A", make_image(dir.filePath("a.png")));
    QFile blocker(p.dir + "/project.3re.tmp-lock");
    const QByteArray before = [&] { QFile f(p.projectFile()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    QFile::setPermissions(p.projectFile(), QFileDevice::ReadOwner);
    p.name = "B";
    bool threw = false;
    try { p.save(); } catch (const std::exception&) { threw = true; }
    QFile::setPermissions(p.projectFile(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray after = [&] { QFile f(p.projectFile()); f.open(QIODevice::ReadOnly); return f.readAll(); }();
    QVERIFY(threw);
    QCOMPARE(after, before);
  }
  void recent_projects_drop_missing() {
    QSettings().remove("projects/recent");
    QTemporaryDir dir;
    const auto a = p3dgui::ThreeReProject::create(dir.filePath("P"), "A", make_image(dir.filePath("a.png")));
    p3dgui::remember_recent(a.projectFile());
    p3dgui::remember_recent(dir.filePath("gone/project.3re"));
    QCOMPARE(p3dgui::recent_projects(), QStringList{a.projectFile()});
    QSettings().remove("projects/recent");
  }
};
THREERE_TEST(ProjectStoreTests)
#include "test_project_store.moc"
