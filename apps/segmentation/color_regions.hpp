#pragma once
// Colour regions: the segmentation that needs no model (pixel art, or when SAM is not
// installed). A click selects the connected area of similar colour.
#include <QImage>
#include <QList>
#include <QPoint>

namespace p3dseg {

// Grayscale8 mask (image size): the 4-connected area within `tolerance` (max channel
// difference, 0-255) of the colour at p; transparent pixels are never included.
QImage color_region_at(const QImage& image, QPoint p, int tolerance = 24);
// Every such area of at least minArea pixels, largest first (at most 24); the background
// (the largest area touching the border) is left out.
QList<QImage> suggest_color_regions(const QImage& image, int minArea, int tolerance = 24);
// A model's mask made usable: every fully enclosed hole is filled (cloth folds, dark
// details) and islands smaller than 0.2 % of the mask's largest part are dropped.
// Openings that reach the outside are kept as they are.
QImage clean_mask(const QImage& mask);
// The original picture with alpha = mask (scaled to the picture); original alpha is kept.
QImage apply_cutout(const QImage& original, const QImage& mask);

}  // namespace p3dseg
