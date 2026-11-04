#include "studio/e2e_driver.hpp"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QFile>
#include <fstream>
#include <nlohmann/json.hpp>
#include <QTimer>

#include "app_controller.hpp"
#include "backends/trellis_backend.hpp"
#include "segmentation/segment_editor.hpp"
#include "studio/angles_panel.hpp"
#include "studio/image3d_workspace.hpp"
#include "studio/retopo_mesh_editor.hpp"
#include "studio/retopo_workspace.hpp"
#include "studio/preview_bar.hpp"
#include "viewport.hpp"
#include "studio/studio_window.hpp"

namespace p3dgui {

void start_retopo_e2e(StudioWindow& studio, RetopoE2EOptions opt) {
  auto& rw = studio.retopoWorkspace();
  auto summary = std::make_shared<QJsonObject>();
  const qint64 started = QDateTime::currentMSecsSinceEpoch();
  (*summary)["model"] = QDir::toNativeSeparators(opt.model);
  (*summary)["app"] = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  (*summary)["started"] = QDateTime::currentDateTime().toString(Qt::ISODate);
  const auto finish = [&studio, opt, summary, started](int code, const QString& why) {
    (*summary)["exit_code"] = code;
    (*summary)["result"] = why;
    (*summary)["total_seconds"] = (QDateTime::currentMSecsSinceEpoch() - started) / 1000.0;
    QSaveFile f(opt.out);
    if (f.open(QIODevice::WriteOnly)) {
      f.write(QJsonDocument(*summary).toJson());
      f.commit();
    }
    studio.pixelBuilder().controller().cleanShutdown();
    QApplication::exit(code);
  };
  const auto shot = [&studio, opt, summary](const QString& suffix) {
    if (opt.shot.isEmpty()) return;
    QApplication::processEvents();
    studio.grab().save(opt.shot + "-" + suffix + ".png");
    // The camera at each shot (it moves only by mouse input).
    const auto& v = studio.retopoWorkspace().editor()->viewport().state();
    QJsonObject cam{{"yaw", v.yaw}, {"pitch", v.pitch}, {"zoom", v.zoom}};
    QJsonObject cams = (*summary)["cameras"].toObject();
    cams[suffix] = cam;
    (*summary)["cameras"] = cams;
  };
  studio.showPage("retopo");
  QObject::connect(&rw, &RetopoWorkspace::finished, &studio, [&studio, &rw, opt, summary, finish, shot](bool ok, const QString& message) mutable {
    if (!ok) return finish(2, message);
    if (!summary->contains("retopo")) {
      (*summary)["retopo"] = QJsonDocument::fromJson(QByteArray::fromStdString(rw.lastReport().dump())).object();
      (*summary)["retopo_status"] = rw.statusText();
      // Before/after pictures through the real viewport, then the saved GLB.
      QTimer::singleShot(800, &studio, [&studio, &rw, opt, summary, finish, shot] {
        shot("retopo");
        rw.editor()->viewport().state().wireframe = true;
        rw.editor()->viewport().refresh();
        QTimer::singleShot(500, &studio, [&studio, &rw, opt, summary, finish, shot] {
          shot("wire");
          rw.editor()->showOriginal(true);
          QTimer::singleShot(800, &studio, [&rw, opt, summary, finish, shot] {
            shot("original");
            rw.editor()->showOriginal(false);
            const QString glb = QFileInfo(opt.out).absolutePath() + "/" + QFileInfo(opt.model).completeBaseName() + "_retopo.glb";
            (*summary)["glb"] = QDir::toNativeSeparators(glb);
            if (!rw.saveGlb(glb)) finish(2, "Save GLB did not start");
          });
        });
      });
      return;
    }
    (*summary)["save_status"] = message;
    try {
      std::ifstream in(std::filesystem::path(((*summary)["glb"].toString() + ".retopo.json").toStdWString()));
      (*summary)["save"] = QJsonDocument::fromJson(QByteArray::fromStdString(nlohmann::json::parse(in).dump())).object();
    } catch (...) {
    }
    // The page now shows the saved GLB with its texture (and the quads with the wireframe on).
    (*summary)["saved_view_shown"] = rw.editor()->showingSaved();
    QTimer::singleShot(800, &studio, [&rw, finish, shot] {
      rw.editor()->viewport().state().wireframe = false;
      rw.editor()->viewport().refresh();
      QTimer::singleShot(500, &rw, [&rw, finish, shot] {
        shot("saved");
        rw.editor()->viewport().state().wireframe = true;
        rw.editor()->viewport().refresh();
        QTimer::singleShot(500, &rw, [finish, shot] {
          shot("saved-wire");
          finish(0, "retopologized, saved and read back");
        });
      });
    });
  });
  QTimer::singleShot(0, &studio, [&studio, &rw, opt, finish] {
    const auto run = [&rw, opt, finish] {
      if (opt.faces > 0) rw.facesBox()->setValue(opt.faces);
      if (!rw.retopologize()) finish(2, "Retopology did not start: " + rw.statusText());
    };
    if (opt.model.endsWith(".3re", Qt::CaseInsensitive)) {
      // A project: its generated result, exactly as "Use Generated Model".
      studio.imageWorkspace().openProject(opt.model);
      auto* poll = new QTimer(&studio);
      auto waited = std::make_shared<int>(0);
      QObject::connect(poll, &QTimer::timeout, &studio, [&studio, &rw, poll, waited, run, finish] {
        if (studio.imageWorkspace().hasResult()) {
          poll->deleteLater();
          rw.refresh();
          studio.showPage("retopo");
          if (!rw.useGenerated()) return finish(2, "Use Generated Model failed");
          run();
        } else if (++*waited > 240) {
          poll->deleteLater();
          finish(2, "The project's result did not open");
        }
      });
      poll->start(250);
    } else {
      if (!rw.openModel(opt.model)) return finish(2, rw.statusText());
      run();
    }
  });
}

E2EDriver::E2EDriver(StudioWindow& studio, E2EOptions opt) : QObject(&studio), studio_(studio), opt_(std::move(opt)) {
  timer_ = new QTimer(this);
  timer_->setInterval(250);
  connect(timer_, &QTimer::timeout, this, &E2EDriver::tick);
}

void E2EDriver::start() {
  started_ = stage_started_ = QDateTime::currentMSecsSinceEpoch();
  summary_["image"] = QDir::toNativeSeparators(opt_.image);
  summary_["app"] = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
  summary_["started"] = QDateTime::currentDateTime().toString(Qt::ISODate);
  auto& ws = studio_.imageWorkspace();
  if (!opt_.project_dir.isEmpty()) ws.setProjectsDir(opt_.project_dir);
  studio_.showPage("image");
  studio_.setImageMode("ai");
  ws.importImage(opt_.image);
  stage_ = 1;
  timer_->start();
}

void E2EDriver::tick() {
  auto& ws = studio_.imageWorkspace();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const double in_stage = (now - stage_started_) / 1000.0;
  auto next = [&](int s) {
    summary_[QStringLiteral("stage_%1_seconds").arg(stage_)] = in_stage;
    stage_ = s;
    stage_started_ = now;
  };
  switch (stage_) {
    case 1:  // picture prepared
      if (ws.statusText().startsWith("Cutout ready")) {
        for (const QString& v : opt_.views) {  // other angles, as the Add Pictures button adds them
          const int eq = int(v.indexOf('='));
          if (eq <= 0) continue;
          if (ws.anglesPanel()->addFiles({v.mid(eq + 1)}) == 1) ws.anglesPanel()->setLabel(int(ws.anglesPanel()->items().size()) - 1, v.left(eq));
        }
        summary_["extra_pictures"] = int(ws.anglesPanel()->items().size());
        if (opt_.region_click || opt_.region_box) {
          ws.showStep(Image3DWorkspace::Segment);
          if (opt_.region_box) ws.segmentEditor()->boxAt(*opt_.region_box);
          else ws.segmentEditor()->clickAt(*opt_.region_click, SegmentEditor::ClickMode::NewRegion);
          next(2);
        } else {
          next(3);
        }
      } else if (in_stage > 60) {
        finish(10, "the picture could not be prepared: " + ws.statusText());
      }
      return;
    case 2:  // SAM region from the click
      if (ws.segmentEditor()->masks() && !ws.segmentEditor()->masks()->regions().isEmpty()) {
        summary_["segmentation_status"] = ws.segmentEditor()->statusText();
        summary_["regions"] = int(ws.segmentEditor()->masks()->regions().size());
        next(3);
      } else if (in_stage > 120) {
        finish(11, "no region from the click: " + ws.segmentEditor()->statusText());
      }
      return;
    case 3:  // GPU check
      ws.showStep(Image3DWorkspace::Generate);
      next(4);
      return;
    case 4: {
      auto* generate = ws.findChild<QPushButton*>("image3dGenerate");
      if (generate && generate->isEnabled()) {
        summary_["gpu_status"] = ws.findChild<QWidget*>("image3dGpuStatus") ? ws.findChild<QLabel*>("image3dGpuStatus")->text() : QString();
        if (opt_.seed) {
          auto* seed = ws.findChild<QSpinBox*>("image3dSeed");
          if (!seed) {
            finish(16, "the seed field was not found");
            return;
          }
          seed->setValue(*opt_.seed);
          summary_["seed"] = seed->value();
        }
        ws.generate();
        next(5);
      } else if (in_stage > 90) {
        finish(12, "generation never became available: " + (generate ? generate->toolTip() : QString()) + " · " + ws.statusText());
      }
      return;
    }
    case 5:  // generation, cleanup, inspection
      if (!ws.backend().busy() && !ws.property("image3dUiBusy").toBool()) {
        summary_["status_text"] = ws.statusText();
        summary_["backend_report"] = ws.backend().report();
        summary_["run_directory"] = QDir::toNativeSeparators(ws.backend().runDirectory());
        if (!ws.hasResult()) {
          finish(13, "generation produced no model: " + ws.statusText());
          return;
        }
        next(6);
      } else if (in_stage > 3600) {
        ws.cancel();
        finish(14, "generation did not finish within an hour");
      }
      return;
    case 6: {  // export + re-import (the Export page's own call)
      const auto& asset = ws.backend().asset();
      summary_["validation"] = QString::fromUtf8(p3dasset::validation_name(asset.validation));
      const QString dir = ws.project() ? ws.project()->dir : QFileInfo(opt_.out).absolutePath();
      const QString glb = QDir(dir).filePath((ws.projectName().isEmpty() ? QStringLiteral("model") : ws.projectName()) + ".glb");
      QString error;
      const bool ok = p3dasset::TrellisBackend::exportAsset(asset, glb, true, error);
      summary_["exported_glb"] = QDir::toNativeSeparators(glb);
      summary_["export_reimport_ok"] = ok;
      if (!ok) summary_["export_error"] = error;
      if (ws.project()) summary_["project"] = QDir::toNativeSeparators(ws.project()->projectFile());
      finish(ok ? 0 : 15, ok ? QStringLiteral("generated and exported") : error);
      return;
    }
  }
}

void E2EDriver::finish(int code, const QString& why) {
  timer_->stop();
  summary_["exit_code"] = code;
  summary_["result"] = why;
  summary_["total_seconds"] = (QDateTime::currentMSecsSinceEpoch() - started_) / 1000.0;
  if (!opt_.shot.isEmpty()) {
    studio_.imageWorkspace().showStep(code == 0 ? Image3DWorkspace::Preview : studio_.imageWorkspace().currentStep());
    QTimer::singleShot(1500, this, [this, code] {
      studio_.grab().save(opt_.shot);
      QSaveFile f(opt_.out);
      if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(summary_).toJson());
        f.commit();
      }
      studio_.pixelBuilder().controller().cleanShutdown();
      QApplication::exit(code);
    });
    return;
  }
  QSaveFile f(opt_.out);
  if (f.open(QIODevice::WriteOnly)) {
    f.write(QJsonDocument(summary_).toJson());
    f.commit();
  }
  studio_.pixelBuilder().controller().cleanShutdown();
  QApplication::exit(code);
}

}  // namespace p3dgui
