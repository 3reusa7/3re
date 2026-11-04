#pragma once
// `3re-studio --e2e-generate PICTURE --e2e-out SUMMARY.json [--project-dir DIR]
// [--region-click X,Y | --region-box X0,Y0,X1,Y1] [--e2e-view LABEL=PICTURE]... [--e2e-seed N] [--e2e-shot PNG]`: the release's own end-to-end check, through the
// real window and workspace code: import, optional SAM region from one click (working
// pixels), GPU check, TRELLIS generation, export + re-import, summary, exit
// (0 = a model was generated and exported).
#include <QObject>
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <optional>

class QTimer;

namespace p3dgui {

class StudioWindow;

// `3re-studio --e2e-retopo MODEL(.glb|.obj|project.3re) --e2e-out SUMMARY.json [--e2e-shot PREFIX] [--retopo-faces N]`:
// Smart Retopology through the real workspace: open the model (a project: its generated result), QuadWild,
// result checks, screenshots (PREFIX-original.png, PREFIX-retopo.png, PREFIX-wire.png), Save GLB + read-back,
// summary, exit (0 = retopologized and saved).
struct RetopoE2EOptions {
  QString model, out, shot;
  int faces = 0;
};
void start_retopo_e2e(StudioWindow& studio, RetopoE2EOptions opt);

struct E2EOptions {
  QString image, out, shot, project_dir;
  std::optional<QPointF> region_click;
  std::optional<QRectF> region_box;
  QStringList views;  // LABEL=PICTURE: other angles added on the Import step (multi-angle, 2.4)
  std::optional<int> seed;  // the Advanced seed (the asset agent regenerates UNSAFE geometry with another seed)
};

class E2EDriver : public QObject {
  Q_OBJECT
 public:
  E2EDriver(StudioWindow& studio, E2EOptions opt);
  void start();

 private:
  void tick();
  void finish(int code, const QString& why);
  StudioWindow& studio_;
  E2EOptions opt_;
  QTimer* timer_ = nullptr;
  int stage_ = 0;
  qint64 stage_started_ = 0, started_ = 0;
  QJsonObject summary_;
};

}  // namespace p3dgui
