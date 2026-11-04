#pragma once
// Image to 3D · other angles (3re 2.3, spec 2026-10-21-multi-angle-design): up to five more pictures of the
// same object or character (back, sides, three-quarter views), each with an angle the user can set or
// correct, and "Split a turnaround sheet" for one picture holding several views side by side. Geometry
// still comes from the front picture; these pictures only texture it. After a texture pass every picture
// shows whether it was used, its outline fit and how much of the surface it owns, or why it was rejected.
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <QWidget>

class QVBoxLayout;
class QPushButton;
class QLabel;

namespace p3dgui {

class AnglesPanel : public QWidget {
  Q_OBJECT
 public:
  struct Item {
    QString file;   // the picture as imported (a project copy once the project exists)
    QString label;  // worker label: back, left, right, front_left, front_right, back_left, back_right, auto, custom:DEG
    QImage cutout;  // prepared like the front picture (background removed)
    QString status; // result of the last texture pass ("" = not used yet)
  };
  static constexpr int kMaxPictures = 5;
  explicit AnglesPanel(QWidget* parent = nullptr);
  const QVector<Item>& items() const { return items_; }
  void setItems(const QVector<Item>& items);
  void clear();
  // Adds pictures (prepares their cutouts); returns how many were added.
  int addFiles(const QStringList& files);
  // Splits a turnaround sheet into its figures (written to `out_dir`), left to right. The first figure is the
  // front; it is returned through `front` when requested, the others are added with default angles.
  static QStringList splitSheet(const QString& sheet, const QString& out_dir, QString* error);
  static QStringList defaultLabels(int figures);  // for figures after the front
  void setResults(const QJsonArray& views, const QJsonObject& ownership);
  void setLabel(int index, const QString& label);
  static QString displayName(const QString& label);
  void setInputType(int type, bool remove_background) { input_type_ = type; remove_background_ = remove_background; }

 signals:
  void changed();
  void sheetSplit(const QString& front_file);

 private:
  void rebuild();
  QVector<Item> items_;
  QVBoxLayout* rows_ = nullptr;
  QPushButton *add_ = nullptr, *split_ = nullptr;
  QLabel* hint_ = nullptr;
  int input_type_ = 0;
  bool remove_background_ = true;
};

}  // namespace p3dgui
