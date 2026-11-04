#pragma once
// The product identity of the desktop application (3re): names, the settings
// store and the one-time move of the Pixel3D-era settings.
class QApplication;
class QSettings;

namespace p3dgui::identity {
inline constexpr const char* kProduct = "3re";
// Organisation/application names, version, icon. Copies the legacy settings once.
void apply(QApplication& app);
// Copies every key of `from` missing in `to`, once (marker identity/migrated). True when it copied.
bool migrate_legacy_settings(QSettings& from, QSettings& to);
}  // namespace p3dgui::identity
