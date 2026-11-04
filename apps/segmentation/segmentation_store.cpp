#include "segmentation/segmentation_store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <stdexcept>

namespace p3dseg {

void SegmentationStore::save(const QString& dir, const MaskSet& set, const QString& imageSha256, QSize originalSize) {
  const QDir d(dir);
  if (!QDir().mkpath(d.filePath("masks"))) throw std::runtime_error("Could not create the segmentation folder");
  QJsonArray regions;
  QStringList keep;
  for (const Region& r : set.regions()) {
    const QString file = QStringLiteral("masks/%1.png").arg(r.id);
    keep << QFileInfo(file).fileName();
    QSaveFile png(d.filePath(file));
    if (!png.open(QIODevice::WriteOnly) || !r.mask.save(&png, "PNG") || !png.commit())
      throw std::runtime_error(("Could not save the region mask " + file).toStdString());
    regions.append(QJsonObject{{"id", r.id}, {"name", r.name}, {"label", r.label}, {"color", r.color.name()},
                               {"include", r.include}, {"visible", r.visible}, {"mask", file}});
  }
  const QJsonObject o{{"format", "3re-segmentation"},
                      {"version", 1},
                      {"image_sha256", imageSha256},
                      {"image_width", originalSize.width()},
                      {"image_height", originalSize.height()},
                      {"mask_width", set.size().width()},
                      {"mask_height", set.size().height()},
                      {"next_id", set.nextId()},
                      {"note", "User-labelled regions (not automatic detection)."},
                      {"regions", regions}};
  QSaveFile json(d.filePath("segmentation.json"));
  if (!json.open(QIODevice::WriteOnly) || json.write(QJsonDocument(o).toJson()) < 0 || !json.commit())
    throw std::runtime_error("Could not save segmentation.json");
  // Masks of deleted regions are no longer referenced by the (already saved) file.
  for (const QString& f : QDir(d.filePath("masks")).entryList({"*.png"}, QDir::Files))
    if (!keep.contains(f)) QFile::remove(d.filePath("masks/" + f));
}

std::unique_ptr<MaskSet> SegmentationStore::load(const QString& dir, const QString& expectedSha256, QString* error) {
  const QDir d(dir);
  QFile f(d.filePath("segmentation.json"));
  if (!f.exists()) return nullptr;
  auto fail = [&](const QString& why) -> std::unique_ptr<MaskSet> {
    if (error) *error = why;
    return nullptr;
  };
  if (!f.open(QIODevice::ReadOnly)) return fail(QObject::tr("The saved regions could not be read."));
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  if (o.value("format").toString() != "3re-segmentation") return fail(QObject::tr("segmentation.json is not a 3re segmentation."));
  if (o.value("version").toInt() > 1) return fail(QObject::tr("These regions were saved by a newer 3re."));
  if (o.value("image_sha256").toString() != expectedSha256)
    return fail(QObject::tr("The picture changed since these regions were saved. They are kept on disk but not applied."));
  const QSize size(o.value("mask_width").toInt(), o.value("mask_height").toInt());
  if (size.isEmpty()) return fail(QObject::tr("The saved regions are damaged (no size)."));
  QList<Region> regions;
  for (const auto& v : o.value("regions").toArray()) {
    const QJsonObject r = v.toObject();
    const QString file = r.value("mask").toString();
    if (file.contains("..")) return fail(QObject::tr("The saved regions are damaged."));
    QImage mask(d.filePath(file));
    if (mask.size() != size) return fail(QObject::tr("A saved region mask is missing or has the wrong size: %1").arg(file));
    Region reg;
    reg.id = r.value("id").toInt();
    reg.name = r.value("name").toString();
    reg.label = r.value("label").toString();
    reg.color = QColor(r.value("color").toString());
    reg.include = r.value("include").toBool(true);
    reg.visible = r.value("visible").toBool(true);
    reg.mask = mask.convertToFormat(QImage::Format_Grayscale8);
    regions << reg;
  }
  auto set = std::make_unique<MaskSet>(size);
  int next = o.value("next_id").toInt(1);
  for (const Region& r : regions) next = std::max(next, r.id + 1);
  set->restore(regions, next);
  if (error) error->clear();
  return set;
}

}  // namespace p3dseg
