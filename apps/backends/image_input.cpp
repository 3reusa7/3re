#include "image_input.hpp"
#include <QImageReader>
#include <QFile>
#include <QPainter>
#include <webp/decode.h>
#include <algorithm>
#include <vector>
#include <limits>
namespace p3dasset {
QSize bounded_image_size(QSize size,int limit){
  if(size.width()<=limit && size.height()<=limit)return size;
  const double scale=double(limit)/std::max(size.width(),size.height());
  return {std::max(1,int(size.width()*scale+.5)),std::max(1,int(size.height()*scale+.5))};
}
ImageInput prepare_image(const QString& file,bool background){return prepare_image(file,ImageInputOptions{InputType::Auto,background});}
ImageInput prepare_image(const QString& file,ImageInputOptions options){
  ImageInput out;out.crisp=options.type==InputType::PixelArt;
  QImageReader reader(file);reader.setAutoTransform(true);out.original_size=reader.size();
  QImage decoded;
  if(!out.original_size.isValid()){
    // Qt's existing installation has no WebP plugin. Reuse Pixel3D's pinned
    // libwebp, with decoder-side scaling instead of an unbounded full decode.
    QFile source(file);
    if(!source.open(QIODevice::ReadOnly) || source.size()>64*1024*1024){out.error="Unsupported, damaged or oversized image data.";return out;}
    const auto bytes=source.readAll();WebPDecoderConfig config{};
    if(!WebPInitDecoderConfig(&config) || WebPGetFeatures(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size()),&config.input)!=VP8_STATUS_OK){out.error="Unsupported or damaged image.";return out;}
    out.original_size={config.input.width,config.input.height};const auto size=bounded_image_size(out.original_size,2048);
    decoded=QImage(size,QImage::Format_RGBA8888);config.output.colorspace=MODE_RGBA;config.output.is_external_memory=1;
    config.output.u.RGBA.rgba=decoded.bits();config.output.u.RGBA.stride=decoded.bytesPerLine();config.output.u.RGBA.size=size_t(decoded.sizeInBytes());
    config.options.use_scaling=1;config.options.scaled_width=size.width();config.options.scaled_height=size.height();config.options.use_threads=0;
    const auto status=WebPDecode(reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size()),&config);WebPFreeDecBuffer(&config.output);
    if(status!=VP8_STATUS_OK){out.error="WebP decoding failed; animated/unsupported data requires a static image.";return out;}
  }
  // Bounded decoder scaling happens before a large destination allocation. The
  // Qt decoder's allocation limit also protects formats requiring a full decode.
  if(!out.crisp)reader.setScaledSize(bounded_image_size(out.original_size,2048));
  auto im=decoded.isNull()?reader.read():decoded;
  if(im.isNull()){out.error="Image decode failed: "+reader.errorString();return out;}
  if(im.width()>2048 || im.height()>2048)im=im.scaled(bounded_image_size(im.size(),2048),Qt::IgnoreAspectRatio,out.crisp?Qt::FastTransformation:Qt::SmoothTransformation);
  im=im.convertToFormat(QImage::Format_RGBA8888);
  bool clear=false,solid=false;
  for(int y=0;y<im.height();++y)for(int x=0;x<im.width();++x){const auto a=im.constScanLine(y)[4*x+3];clear|=a<128;solid|=a>=128;}
  if(!solid){out.error="The image is fully transparent.";return out;}
  if(!clear && options.automatic_background){
    // Remove only a consistent exterior background. No semantic segmentation is
    // claimed for complex photographs, white object highlights stay enclosed.
    const auto bg=im.pixelColor(0,0);
    auto close=[&](QColor c){return std::max({std::abs(c.red()-bg.red()),std::abs(c.green()-bg.green()),std::abs(c.blue()-bg.blue())})<=12;};
    int border=0,matching=0;
    for(int x=0;x<im.width();++x){border+=2;matching+=close(im.pixelColor(x,0))+close(im.pixelColor(x,im.height()-1));}
    for(int y=0;y<im.height();++y){border+=2;matching+=close(im.pixelColor(0,y))+close(im.pixelColor(im.width()-1,y));}
    if(matching>border*.9){
      std::vector<uint8_t> visited(size_t(im.width())*im.height());std::vector<int> queue;queue.reserve(visited.size());
      auto add=[&](int x,int y){const int i=y*im.width()+x;if(!visited[size_t(i)] && close(im.pixelColor(x,y))){visited[size_t(i)]=1;queue.push_back(i);}};
      for(int x=0;x<im.width();++x){add(x,0);add(x,im.height()-1);}for(int y=0;y<im.height();++y){add(0,y);add(im.width()-1,y);}
      for(size_t i=0;i<queue.size();++i){const int x=queue[i]%im.width(),y=queue[i]/im.width();im.scanLine(y)[4*x+3]=0;if(x)add(x-1,y);if(y)add(x,y-1);if(x+1<im.width())add(x+1,y);if(y+1<im.height())add(x,y+1);}
      // A uniform image is still a valid opaque source; do not erase it entirely.
      if(queue.size()==visited.size()){im.fill(bg);out.warning="No distinct object found; retaining the full image.";}
      else {clear=true;out.warning="Uniform exterior background removed; review thin parts and matching subject colors.";}
    }
  }
  int x0=im.width(),y0=im.height(),x1=-1,y1=-1;
  for(int y=0;y<im.height();++y)for(int x=0;x<im.width();++x)if(im.constScanLine(y)[4*x+3]>=128){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);}
  if(x1<0){out.error="No visible subject remains.";return out;}
  out.foreground_touches_border=x0==0 || y0==0 || x1==im.width()-1 || y1==im.height()-1;
  out.subject_bounds=QRect(x0,y0,x1-x0+1,y1-y0+1);
  if(!clear && out.warning.isEmpty())out.warning="Complex/opaque background retained. Full-image framing is approximate; isolated subjects give better geometry.";
  const auto subject=im.copy(out.subject_bounds);
  out.appearance=subject; // Preserve source detail before the 1024 inference framing step.
  const int side=std::min(1024,std::max(subject.width(),subject.height()));
  const int frame=std::max(16,side+std::max(4,side/10));
  out.cutout=QImage(frame,frame,QImage::Format_RGBA8888);out.cutout.fill(Qt::transparent);
  const auto scaled=subject.scaled(bounded_image_size(subject.size(),side),Qt::IgnoreAspectRatio,out.crisp?Qt::FastTransformation:Qt::SmoothTransformation);
  QPainter painter(&out.cutout);painter.drawImage((frame-scaled.width())/2,(frame-scaled.height())/2,scaled);painter.end();
  return out;
}
}
