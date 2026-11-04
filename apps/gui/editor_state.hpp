#pragma once
// What the user is doing: the active tool and its options. Shared by the 3D
// viewport, the drawing preview, the tool bar over the viewport and the
// inspector. It holds no document state; every edit still goes to
// AppController as one Session command.
#include <QObject>
#include <QString>

#include "pixel3d/edit/brush.hpp"
#include "pixel3d/edit/selection.hpp"

namespace p3dgui {

// Select: pick parts/faces. Depth: depth brush on a drawing (and the depth
// questions the drawings leave open). Push/Pull: drag a face. Volume: a
// rectangle on a drawing becomes added/erased/extruded volume. Parts: part
// list and the part-mask brush.
enum class Tool { Select, Depth, PushPull, Volume, Parts };
QString tool_name(Tool t);
QString tool_hint(Tool t);

// How the mouse moves the camera.
enum class CameraScheme {
  Pixel3D,  // left drag orbits (when the tool does not drag), right drag pans, middle orbits, wheel zooms
  Blender,  // middle drag orbits, Shift+middle pans, Ctrl+middle zooms
  Maya,     // Alt+left orbits, Alt+middle pans, Alt+right zooms
};
QString camera_scheme_name(CameraScheme s);

class EditorState : public QObject {
  Q_OBJECT
 public:
  using QObject::QObject;

  Tool tool() const { return tool_; }
  void setTool(Tool t);
  pixel3d::SelectMode selectMode() const { return mode_; }
  void setSelectMode(pixel3d::SelectMode m);
  CameraScheme cameraScheme() const { return scheme_; }
  void setCameraScheme(CameraScheme s);

  // Tool options (read by the tools; touch() after changing them).
  pixel3d::BrushParams depth_brush{1.5, 1.0, 0.5, false, 1.0};
  bool depth_selected_only = false;  // the depth brush changes only the selected part
  double mask_radius = 0.5;
  bool mask_erase = false;
  int mask_part = -1;                // the user part the mask brush paints
  bool snap = true;                  // whole pixels for push/pull and transforms
  bool respect_bounds = true;        // push/pull stops at the drawings' box
  double push_distance = 1;          // numeric push/pull distance

  void touch() { emit changed(); }

 signals:
  void changed();

 private:
  Tool tool_ = Tool::Select;
  pixel3d::SelectMode mode_ = pixel3d::SelectMode::Part;
  CameraScheme scheme_ = CameraScheme::Pixel3D;
};

}  // namespace p3dgui
