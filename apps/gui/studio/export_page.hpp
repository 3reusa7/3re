#pragma once
// Export: the current results (Image to 3D, Pixel Art Builder), validation in words,
// GLB export that re-imports and checks, and an explicit confirmation for non-PASS models.
#include <QString>
#include <QWidget>
#include <functional>

#include "backends/mesh_asset.hpp"

template <typename T>
class QFutureWatcher;
class QLabel;
class QPushButton;

namespace p3dgui {

class Card;
class Image3DWorkspace;
class MainWindow;

class ExportPage : public QWidget {
  Q_OBJECT
 public:
  explicit ExportPage(QWidget* parent = nullptr);
  void setSources(Image3DWorkspace* ai, MainWindow* pixel);
  void refresh();
  void exportGenerated();
  // Confirmation for non-PASS, save prompt, then export + re-import in the background.
  void exportAsset(const p3dasset::MeshAsset& asset, const QString& name, const QString& folder);
  // Test hooks: the confirmation for non-PASS models and the save-path prompt.
  void setAcknowledgeHandler(std::function<bool(const QString&)> f) { acknowledge_ = std::move(f); }
  void setSavePathPrompt(std::function<QString(const QString&)> f) { ask_path_ = std::move(f); }
  QString lastExport() const { return last_export_; }

 signals:
  void goToImage();

 private:
  Image3DWorkspace* ai_ = nullptr;
  MainWindow* pixel_ = nullptr;
  Card *empty_ = nullptr, *ai_card_ = nullptr, *pixel_card_ = nullptr;
  QLabel *ai_name_ = nullptr, *ai_chip_ = nullptr, *ai_text_ = nullptr, *ai_status_ = nullptr, *pixel_text_ = nullptr;
  QPushButton *ai_export_ = nullptr, *ai_folder_ = nullptr;
  QFutureWatcher<QString>* job_ = nullptr;
  std::function<bool(const QString&)> acknowledge_;
  std::function<QString(const QString&)> ask_path_;
  QString last_export_;
};

}  // namespace p3dgui
