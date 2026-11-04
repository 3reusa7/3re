#pragma once
// Regions on disk inside a 3re project: segmentation/segmentation.json + masks/<id>.png.
// A load is refused (with a message, files kept) when the picture's SHA-256 changed.
#include <QSize>
#include <QString>
#include <memory>

#include "segmentation/mask_set.hpp"

namespace p3dseg {

struct SegmentationStore {
  // Throws std::runtime_error when the files cannot be written (each file is written atomically).
  static void save(const QString& dir, const MaskSet& set, const QString& imageSha256, QSize originalSize);
  // nullptr with an empty error when nothing was saved; nullptr with a message when refused.
  static std::unique_ptr<MaskSet> load(const QString& dir, const QString& expectedSha256, QString* error);
};

}  // namespace p3dseg
