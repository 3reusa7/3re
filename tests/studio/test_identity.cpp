#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "app_identity.hpp"
#include "backends/settings_env.hpp"
#include "registry.hpp"
#include "pixel3d/common/version.hpp"

class IdentityTests : public QObject {
  Q_OBJECT
 private slots:
  void migrate_copies_once() {
    QTemporaryDir dir;
    QSettings legacy(dir.filePath("legacy.ini"), QSettings::IniFormat), fresh(dir.filePath("3re.ini"), QSettings::IniFormat);
    legacy.setValue("trellis/setup", "C:/x/backend.json");
    legacy.setValue("recent/files", QStringList{"a.p3d", "b.p3d"});
    QVERIFY(p3dgui::identity::migrate_legacy_settings(legacy, fresh));
    QCOMPARE(fresh.value("trellis/setup").toString(), QString("C:/x/backend.json"));
    QCOMPARE(fresh.value("recent/files").toStringList(), (QStringList{"a.p3d", "b.p3d"}));
    QVERIFY(fresh.value("identity/migrated").toBool());
    legacy.setValue("trellis/setup", "changed");
    QVERIFY(!p3dgui::identity::migrate_legacy_settings(legacy, fresh));
    QCOMPARE(fresh.value("trellis/setup").toString(), QString("C:/x/backend.json"));
  }
  void migrate_never_overwrites_new_values() {
    QTemporaryDir dir;
    QSettings legacy(dir.filePath("legacy.ini"), QSettings::IniFormat), fresh(dir.filePath("3re.ini"), QSettings::IniFormat);
    legacy.setValue("ui/scale", 1.0);
    fresh.setValue("ui/scale", 1.5);
    p3dgui::identity::migrate_legacy_settings(legacy, fresh);
    QCOMPARE(fresh.value("ui/scale").toDouble(), 1.5);
  }
  void env_prefers_threere() {
    qputenv("THREERE_TEST_VALUE", "new");
    qputenv("PIXEL3D_TEST_VALUE", "old");
    QCOMPARE(p3dasset::setting_env("TEST_VALUE"), QString("new"));
    qunsetenv("THREERE_TEST_VALUE");
    QCOMPARE(p3dasset::setting_env("TEST_VALUE"), QString("old"));
    qunsetenv("PIXEL3D_TEST_VALUE");
    QVERIFY(p3dasset::setting_env("TEST_VALUE").isEmpty());
  }
  void version_is_2_8_0_alpha() {
    QCOMPARE(QString::fromUtf8(pixel3d::version().data(), qsizetype(pixel3d::version().size())), QString("2.10.0-alpha"));
    QCOMPARE(QString(p3dgui::identity::kProduct), QString("3re"));
  }
};
THREERE_TEST(IdentityTests)
#include "test_identity.moc"
