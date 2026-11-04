#pragma once
// Settings: generation setup and projects folder, display, advanced options, About, diagnostics.
#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
namespace p3dasset { class RetextureBatch; }

namespace p3dgui {

class SettingsPage : public QWidget {
  Q_OBJECT
 public:
  explicit SettingsPage(QWidget* parent = nullptr);
  // Interface size choices (0 = follow Windows), as stored in ui/scale and applied at start.
  static const QList<double>& scales();
  void setDiagnostics(const QString& text);
  // Re-texture all saved models (background, cancellable); also used by --retexture-all.
  void startRetextureAll();
  bool retextureRunning() const;
  // Delivery check: where Godot 4 is (Settings path, else found automatically) and whether it answers.
  void refreshGodot();

 signals:
  void advancedChanged(bool on);

 private:
  void saveGodot();
  QLineEdit* projects_ = nullptr;
  QLineEdit* godot_ = nullptr;
  QLabel* godot_status_ = nullptr;
  QComboBox* scale_ = nullptr;
  QCheckBox *software_ = nullptr, *advanced_ = nullptr;
  QVBoxLayout* diagnostics_ = nullptr;
  QLabel* diag_text_ = nullptr;
  p3dasset::RetextureBatch* batch_ = nullptr;
  QPushButton *retex_ = nullptr, *retex_cancel_ = nullptr;
  QProgressBar* retex_bar_ = nullptr;
  QLabel* retex_text_ = nullptr;
};

}  // namespace p3dgui
