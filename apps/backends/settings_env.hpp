#pragma once
// 3re environment settings: THREERE_<NAME> wins, the Pixel3D-era PIXEL3D_<NAME> is still read.
#include <QString>
#include <QtGlobal>

namespace p3dasset {
inline QString setting_env(const char* name) {
  const QString preferred = qEnvironmentVariable((QByteArray("THREERE_") + name).constData());
  return preferred.isEmpty() ? qEnvironmentVariable((QByteArray("PIXEL3D_") + name).constData()) : preferred;
}
}  // namespace p3dasset
