#pragma once
// Home: the two ways to start, recent projects and what this computer can do.
#include <QJsonObject>
#include <QWidget>

template <typename T>
class QFutureWatcher;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace p3dgui {

class HomePage : public QWidget {
  Q_OBJECT
 public:
  explicit HomePage(QWidget* parent = nullptr);
  void refresh();

 signals:
  void startImage();
  void startPixel();
  void openImageProject(const QString& projectFile);
  void openPixelProject(const QString& p3dFile);

 private:
  void checkGpu();
  QWidget* recent_list_ = nullptr;
  QVBoxLayout* recent_layout_ = nullptr;
  QLabel *models_chip_ = nullptr, *models_detail_ = nullptr, *gpu_chip_ = nullptr, *gpu_detail_ = nullptr;
  QLabel *ram_chip_ = nullptr, *ram_detail_ = nullptr, *sam_chip_ = nullptr, *sam_detail_ = nullptr;
  QPushButton* gpu_check_ = nullptr;
  QFutureWatcher<QJsonObject>* gpu_job_ = nullptr;
};

}  // namespace p3dgui
