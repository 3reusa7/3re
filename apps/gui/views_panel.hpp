#pragma once
// Left panel: one card per source drawing (thumbnail, name, orientation,
// resolution, assignment state, issue marks; drop a PNG on a card to assign
// it) and, below, a pixel-exact preview of the selected drawing with
// optional overlays (comparison with the model, issue regions, pixel grid,
// part masks, the selection) on which the editing tools work: a click picks
// a pixel, a drag paints a stroke or a rectangle, in drawing pixels.
#include <QFrame>
#include <QImage>
#include <QStringList>
#include <QVector>
#include <set>
#include <array>
#include <map>
#include <optional>
#include <vector>

#include "pixel3d/engine/job_runner.hpp"
#include "widgets.hpp"

class QLabel;
class QPushButton;
class QToolButton;

namespace p3dgui {

class Section;
class EditorState;

// A stable display colour for a user part id.
QColor part_color(int part);

// Guesses a view role from a file name (front, side/left, top, back, right, bottom).
std::optional<pixel3d::ViewRole> role_from_filename(const QString& path);

// Regions to mark per drawing (aligned image pixels).
using ViewMarks = std::map<pixel3d::ViewRole, std::vector<QRectF>>;

// Everything a card or slot shows about one drawing.
struct DrawingInfo {
  pixel3d::ViewRole role = pixel3d::ViewRole::Front;
  bool assigned = false;
  QImage image;  // aligned if available, else original
  QString file;
  QSize size;    // original pixels
  QString notes; // "scaled ÷2", "mirrored"
  bool optional = false;  // the project builds without it (Back/Right/Bottom; Side and Top in Low-Poly Studio)
};
DrawingInfo drawing_info(const pixel3d::SessionView& v, pixel3d::ViewRole r);
// Draws `img` pixel-exact (integer zoom) centred in `area` over a checkerboard; returns the image rect.
QRectF paint_pixel_image(class QPainter& p, const QImage& img, const QRectF& area, int max_zoom = 64);

// A visual-hull project without its Top (or Side) drawing: what is missing, and the one-click way out
// (switch to Low-Poly Studio, keeping the drawings) or the missing drawing. Hidden otherwise.
class MissingViewsNotice : public Card {
  Q_OBJECT
 public:
  // narrow: the button text on two lines (the Views panel is 260 px wide at least).
  explicit MissingViewsNotice(bool narrow = false, QWidget* parent = nullptr);
  void update_from(const pixel3d::Document& d);
  QPushButton* switchButton() const { return switch_; }
  QPushButton* addButton() const { return add_; }

 signals:
  void switchRequested();
  void addRequested(pixel3d::ViewRole role);

 private:
  QLabel* text_ = nullptr;
  QPushButton* switch_ = nullptr;
  QPushButton* add_ = nullptr;
  pixel3d::ViewRole add_role_ = pixel3d::ViewRole::Top;
};

class ViewCard : public QFrame {
  Q_OBJECT
 public:
  explicit ViewCard(pixel3d::ViewRole role, QWidget* parent = nullptr);
  pixel3d::ViewRole role() const { return info_.role; }
  void setInfo(const DrawingInfo& info);
  void setMarks(const std::vector<QRectF>& rects, int issues);
  void setSelected(bool on);
  QSize sizeHint() const override;

 signals:
  void clickedCard(pixel3d::ViewRole role);
  void browse(pixel3d::ViewRole role);
  void filesDropped(const QStringList& files, pixel3d::ViewRole role);

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragLeaveEvent(QDragLeaveEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  void enterEvent(QEnterEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  DrawingInfo info_;
  std::vector<QRectF> marks_;
  int issues_ = 0;
  bool selected_ = false, drag_over_ = false, hover_ = false;
};

// Pixel overlays of the editor, in drawing pixels.
struct EditMarks {
  QImage masks;      // part masks, coloured per part (transparent elsewhere)
  QImage selection;  // the selection projected into this drawing
  QImage unmatched;  // mask pixels the part's geometry does not cover
  QImage pending;    // a mask or depth stroke in progress
};

class SourcePreview : public QWidget {
  Q_OBJECT
 public:
  enum class Input { None, Click, Brush, Rect };
  explicit SourcePreview(QWidget* parent = nullptr);
  void setImage(const QImage& img, const QString& empty_text);
  void setCompare(const QImage& img);
  void setMarks(const std::vector<QRectF>& issues, const std::vector<QRectF>& focus, const std::vector<QRectF>& accent);
  void setOptions(bool compare, bool issues, bool grid);
  void setEditMarks(const EditMarks& m);
  void setInput(Input mode, double brush_radius);
  // Widget position of a drawing pixel coordinate (tests, cursor).
  QPoint widgetPos(QPointF pixel) const;

 signals:
  void clicked(QPointF pixel);
  void stroke(const QVector<QPointF>& points, bool finished);
  void rectDragged(QRect pixels);

 protected:
  void paintEvent(QPaintEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  QPointF toPixel(QPointF pos) const;
  QImage img_, compare_;
  QString empty_text_;
  std::vector<QRectF> issues_, focus_, accent_;
  bool show_compare_ = false, show_issues_ = true, show_grid_ = true;
  EditMarks edit_;
  Input input_ = Input::Click;
  double brush_radius_ = 0;
  QRectF img_rect_;  // where the drawing was painted (logical pixels)
  bool pressed_ = false;
  QPointF press_px_, hover_{-1, -1};
  QVector<QPointF> points_;
};

class ViewsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit ViewsPanel(QWidget* parent = nullptr);
  void update_from(const pixel3d::SessionView& v);
  // all: every open issue; focus: the opened issue; accent: hovered candidate;
  // counts: open issues per drawing (card badge).
  void setMarks(const ViewMarks& all, const ViewMarks& focus, const ViewMarks& accent,
                const std::map<pixel3d::ViewRole, int>& counts);
  void setRole(pixel3d::ViewRole r);
  pixel3d::ViewRole role() const { return role_; }
  void setCompare(int role, const QImage& img, const QString& counts_json);
  bool compareOn() const;
  // Editing: the tool decides what the preview's mouse does; the window's
  // selection and strokes in progress are drawn on the drawing.
  void setEditor(EditorState* e);
  void setSelection(const std::set<int>& parts, int region);
  void setPending(const QVector<QPoint>& pixels, const QColor& color);
  SourcePreview* preview() const { return preview_; }
  MissingViewsNotice* missingNotice() const { return missing_; }

 signals:
  void filesDropped(const QStringList& files, std::optional<pixel3d::ViewRole> role);
  void browseRequested(pixel3d::ViewRole role);
  void compareRequested(pixel3d::ViewRole role);
  void roleChanged(pixel3d::ViewRole role);
  void pixelClicked(pixel3d::ViewRole role, int u, int v);
  void strokeChanged(pixel3d::ViewRole role, const QVector<QPointF>& points, bool finished);
  void regionChosen(pixel3d::ViewRole role, QRect pixels);
  void projectTypeRequested(bool lowpoly);  // the missing-views notice's switch

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  void refreshPreview();

  std::array<ViewCard*, 6> cards_{};
  MissingViewsNotice* missing_ = nullptr;
  Section* more_ = nullptr;
  SourcePreview* preview_ = nullptr;
  QLabel* preview_title_ = nullptr;
  QLabel* legend_ = nullptr;
  QToolButton* compare_ = nullptr;
  QToolButton* issues_ = nullptr;
  QToolButton* grid_ = nullptr;
  pixel3d::ViewRole role_ = pixel3d::ViewRole::Front;
  pixel3d::SessionView view_;
  ViewMarks all_, focus_, accent_;
  std::map<int, QImage> compare_img_;
  std::map<int, QString> counts_;
  bool more_auto_opened_ = false;
  EditorState* editor_ = nullptr;
  std::set<int> sel_parts_;
  int sel_region_ = -1;
  QVector<QPoint> pending_;
  QColor pending_color_;
};

}  // namespace p3dgui
