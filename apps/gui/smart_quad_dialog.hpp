#pragma once
// Imported assets have their own state: Smart Quad never changes the drawing project.
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPixmap>
#include <QProcess>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimer;

namespace p3dgui {

class SmartQuadDialog : public QDialog {
  Q_OBJECT
 public:
  explicit SmartQuadDialog(QWidget* parent = nullptr);
  ~SmartQuadDialog() override;
  QJsonObject request() const;
  bool isRunning() const { return process_.state() != QProcess::NotRunning; }
  QString statusText() const;
  static bool acceptedReport(const QJsonObject& report);

 public slots:
  void start();
  void cancel();
  void reject() override;

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void setRunning(bool running);
  void setStatus(const QString& text, bool error = false);
  void readOutput();
  void consumeLine(const QByteArray& line);
  void processFinished(int code, QProcess::ExitStatus exitStatus);
  void stopProcess(const QString& reason);
  void checkResources();
  void loadReport(bool processOk);
  void refreshPreview();
  bool containProcess();
  void releaseJob();
  QString pipelinePath() const;
  QString auditPath() const;
  QString checkPath() const;
  QString newOutputPath(const QString& parent) const;

  QLineEdit* input_ = nullptr;
  QLineEdit* output_ = nullptr;
  QLineEdit* blender_ = nullptr;
  QLineEdit* remesher_ = nullptr;
  QSpinBox* budget_ = nullptr;
  QSpinBox* lods_ = nullptr;
  QCheckBox* fidelity_ = nullptr;
  QCheckBox* shape_ = nullptr;
  QCheckBox* materials_ = nullptr;
  QCheckBox* sharp_ = nullptr;
  QWidget* controls_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* stats_ = nullptr;
  QLabel* preview_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QPushButton* run_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QPushButton* folder_ = nullptr;
  QPushButton* game_ = nullptr;
  QTimer* watchdog_ = nullptr;
  QProcess process_;
  QElapsedTimer elapsed_;
  QByteArray buffered_;
  QPixmap preview_image_;
  QString run_output_;
  QString game_path_;
  QString stop_reason_;
  void* job_ = nullptr;  // Windows Job Object; closing it terminates the whole worker tree.
};

}  // namespace p3dgui
