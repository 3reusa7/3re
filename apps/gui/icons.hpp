#pragma once
// One custom vector icon set for the whole application: thin round strokes on
// a 24-unit grid, painted at any size (crisp at every display scale) and tinted
// from the theme per icon mode (normal, hover, disabled, checked).
#include <QIcon>

#include "theme.hpp"

namespace p3dgui {

enum class Icon {
  // Primary actions and tools
  Import, Reconstruct, Select, Depth, PushPull, Parts, Mirror, Validate, Export,
  // File / history
  New, Open, Save, Undo, Redo,
  // Cameras and display
  ViewFront, ViewSide, ViewTop, View3D, Wireframe, Lighting, Grid, DrawingOverlay, IssuesOverlay,
  EstimatedOverlay, Turntable, Frame,
  // Status and UI
  StatusPass, StatusReview, StatusFail, StatusNone, Info, ChevronRight, ChevronDown, ChevronLeft, Close,
  More, Log, Settings, Compare, PixelGrid, Drop,
  // M4 editor
  Volume, Brush, Normals, Ortho, Texture, PixelPreview, History, Snap,
  // 3re studio: workspaces and segmentation tools
  Home, ImageTo3D, Retopo, Bone, Animation, Regions, Eraser, BoxSelect, ClickSelect, Folder
};

enum class IconStyle {
  Default,   // secondary text colour; brighter on hover; accent when checked
  OnAccent,  // white, for primary (accent-filled) buttons
  Toned,     // fixed semantic colour (status icons)
};

QIcon icon(Icon id, IconStyle style = IconStyle::Default, theme::Tone tone = theme::Tone::Neutral);
// Status icon for a validation status (check / exclamation / cross / dash).
QIcon status_icon(pixel3d::ValidationStatus s);

}  // namespace p3dgui
