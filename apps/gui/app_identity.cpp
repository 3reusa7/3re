#include "app_identity.hpp"

#include <QApplication>
#include <QIcon>
#include <QSettings>

#include "pixel3d/common/version.hpp"

namespace p3dgui::identity {

bool migrate_legacy_settings(QSettings& from, QSettings& to) {
  if (to.value("identity/migrated").toBool()) return false;
  for (const QString& key : from.allKeys())
    if (!to.contains(key)) to.setValue(key, from.value(key));
  to.setValue("identity/migrated", true);
  to.sync();
  return true;
}

void apply(QApplication& app) {
  QApplication::setOrganizationName(kProduct);
  QApplication::setApplicationName(kProduct);
  QApplication::setApplicationDisplayName(kProduct);
  const auto v = pixel3d::version();
  QApplication::setApplicationVersion(QString::fromUtf8(v.data(), qsizetype(v.size())));
  app.setWindowIcon(QIcon(QStringLiteral(":/3re.png")));
  if (QSettings::defaultFormat() == QSettings::NativeFormat) {
    QSettings legacy("Pixel3D", "Pixel3D"), current;
    migrate_legacy_settings(legacy, current);
  }
}

}  // namespace p3dgui::identity
