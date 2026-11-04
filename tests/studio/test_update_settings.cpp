// Settings > Updates and AI models (docs/UPDATES.md): the controls exist, they store their choice, and a development
// build (no release folder) says it does not update itself instead of offering a check that cannot work.
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTest>

#include "registry.hpp"
#include "studio/settings_page.hpp"
#include "update/update_service.hpp"

class UpdateSettingsTests : public QObject {
  Q_OBJECT
 private slots:
  void controls_store_the_choice() {
    QSettings().remove("update");
    p3dgui::SettingsPage page;
    auto* channel = page.findChild<QComboBox*>("settingsUpdateChannel");
    auto* automatic = page.findChild<QCheckBox*>("settingsUpdateAuto");
    QVERIFY(channel && automatic);
    QVERIFY(automatic->isChecked());  // on by default, and it can be turned off
    channel->setCurrentIndex(channel->findData("stable"));
    QCOMPARE(threere::update::UpdateService::channel(), QStringLiteral("stable"));
    automatic->setChecked(false);
    QVERIFY(!threere::update::UpdateService::autoCheck());
    QVERIFY(!threere::update::UpdateService::checkDue());
  }
  void development_build_does_not_offer_updates_or_downloads() {
    p3dgui::SettingsPage page;
    auto* check = page.findChild<QPushButton*>("settingsUpdateCheck");
    auto* status = page.findChild<QLabel*>("settingsUpdateStatus");
    auto* models = page.findChild<QPushButton*>("settingsModelsDownload");
    QVERIFY(check && status && models);
    QVERIFY(!check->isEnabled());
    QVERIFY2(status->text().contains("does not update itself"), qPrintable(status->text()));
    QVERIFY(!models->isEnabled());
  }
};
THREERE_TEST(UpdateSettingsTests)
#include "test_update_settings.moc"
