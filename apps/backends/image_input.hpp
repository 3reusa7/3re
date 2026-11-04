#pragma once
#include <QImage>
#include <QRect>
#include <QString>
namespace p3dasset {
enum class InputType { Auto, Photo, Illustration, Character, PixelArt };
struct ImageInputOptions { InputType type=InputType::Auto; bool automatic_background=true; };
struct ImageInput {
  QImage cutout; QImage appearance; // Up to 2048 source pixels; independent of the neural cutout.
  QString error, warning; bool foreground_touches_border=false;
  QSize original_size; QRect subject_bounds; bool crisp=false;
};
QSize bounded_image_size(QSize size,int limit);
ImageInput prepare_image(const QString& file,ImageInputOptions options={});
ImageInput prepare_image(const QString& file,bool remove_background);
}
