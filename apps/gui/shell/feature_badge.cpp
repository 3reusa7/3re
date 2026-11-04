#include "shell/feature_badge.hpp"

#include "widgets.hpp"

namespace p3dgui {

QLabel* make_feature_badge(Feature f, QWidget* parent) {
  auto* chip = f == Feature::Experimental ? make_chip(QObject::tr("Experimental"), theme::Tone::Review, parent)
                                          : make_chip(QObject::tr("Coming Soon"), theme::Tone::Neutral, parent);
  chip->setToolTip(f == Feature::Experimental
                       ? QObject::tr("Works, but results can need review. Check the result before you rely on it.")
                       : QObject::tr("Not available in this version."));
  chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  return chip;
}

}  // namespace p3dgui
