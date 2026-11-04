#pragma once
// Small conversions between core types and Qt types (GUI only).
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

#include <nlohmann/json.hpp>

#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/session.hpp"

namespace p3dgui {

inline QImage to_qimage(const pixel3d::ImageRGBA& img) {
  QImage out(img.width(), img.height(), QImage::Format_RGBA8888);
  for (int y = 0; y < img.height(); ++y) {
    auto* line = reinterpret_cast<pixel3d::Rgba8*>(out.scanLine(y));
    for (int x = 0; x < img.width(); ++x) line[x] = img.at(x, y);
  }
  return out;
}

inline QString qs(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
inline QString qs(std::string_view s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }
inline std::string utf8(const QString& s) { return s.toUtf8().toStdString(); }
inline std::filesystem::path to_path(const QString& s) { return pixel3d::path_from_utf8(utf8(s)); }
inline QString from_path(const std::filesystem::path& p) { return qs(pixel3d::to_utf8(p)); }

// Source image files the engine can read (by extension) and the file-dialog filter for them.
inline const QStringList& source_image_extensions() {
  static const QStringList ext{"png", "jpg", "jpeg", "bmp", "gif", "tga", "webp", "aseprite", "ase", "psd"};
  return ext;
}
// Layered drawings: their layers can become suggested parts.
inline bool is_layered_source(const QString& path) {
  const QString e = path.mid(path.lastIndexOf('.') + 1).toLower();
  return path.contains('.') && (e == "aseprite" || e == "ase" || e == "psd");
}
inline bool is_source_image(const QString& path) {
  const qsizetype dot = path.lastIndexOf('.');
  return dot >= 0 && source_image_extensions().contains(path.mid(dot + 1).toLower());
}
inline QString source_image_filter() {
  QStringList pats;
  for (const QString& e : source_image_extensions()) pats << "*." + e;
  return QObject::tr("Drawings (%1)").arg(pats.join(' '));
}

inline QString json_text(const nlohmann::json& j, int indent = 2) {
  return qs(j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace));
}




}  // namespace p3dgui
