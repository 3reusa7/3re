#pragma once
// Rigging: the Low-Poly Studio automatic rig (Experimental) when the Pixel Art Builder
// holds a built low-poly character; otherwise Coming Soon (generated meshes are not rigged).
#include <QWidget>

#include "main_window.hpp"

class QCheckBox;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace p3dgui {

class RiggingPage : public QWidget {
  Q_OBJECT
 public:
  RiggingPage(MainWindow& pixel, QWidget* parent = nullptr);
  void refresh();

 private:
  MainWindow& pixel_;
  QStackedWidget* stack_ = nullptr;
  QLabel* summary_ = nullptr;
  QCheckBox* pose_ = nullptr;
  QPushButton* exportRigged_ = nullptr;
};

}  // namespace p3dgui
