#include "editor_state.hpp"

namespace p3dgui {

QString tool_name(Tool t) {
  switch (t) {
    case Tool::Select: return QObject::tr("Select");
    case Tool::Depth: return QObject::tr("Depth");
    case Tool::PushPull: return QObject::tr("Push/Pull");
    case Tool::Volume: return QObject::tr("Volume");
    case Tool::Parts: return QObject::tr("Parts");
  }
  return {};
}

QString tool_hint(Tool t) {
  switch (t) {
    case Tool::Select: return QObject::tr("Click a part or face — in the model or on a drawing");
    case Tool::Depth: return QObject::tr("Paint on a drawing to raise or lower the surface; click an amber area to decide its depth");
    case Tool::PushPull: return QObject::tr("Drag a face along its normal (up/down when it faces you)");
    case Tool::Volume: return QObject::tr("Drag a rectangle on a drawing, then add, erase or extrude");
    case Tool::Parts: return QObject::tr("Click a part to inspect it; paint a part's pixels on a drawing");
  }
  return {};
}

QString camera_scheme_name(CameraScheme s) {
  switch (s) {
    case CameraScheme::Pixel3D: return QObject::tr("Pixel3D (left orbit, right pan)");
    case CameraScheme::Blender: return QObject::tr("Blender (middle orbit, Shift+middle pan)");
    case CameraScheme::Maya: return QObject::tr("Maya (Alt+left orbit, Alt+middle pan)");
  }
  return {};
}

void EditorState::setTool(Tool t) {
  if (t == tool_) return;
  tool_ = t;
  emit changed();
}

void EditorState::setSelectMode(pixel3d::SelectMode m) {
  if (m == mode_) return;
  mode_ = m;
  emit changed();
}

void EditorState::setCameraScheme(CameraScheme s) {
  if (s == scheme_) return;
  scheme_ = s;
  emit changed();
}

}  // namespace p3dgui
