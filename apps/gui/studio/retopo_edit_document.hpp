#pragma once
// Retopology-local edit history. Never welds vertices or changes face membership.
// Coordinates remain in source units; rendering transforms belong to the view.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>
#include "pixel3d/mesh/mesh.hpp"

namespace p3dgui {
class RetopoEditDocument {
 public:
  using Face = std::vector<uint32_t>;
  enum class Transform { Move, Rotate, Scale };
  void load(std::vector<pixel3d::vec3d> positions, std::vector<Face> faces) {
    validate(positions, faces);
    positions_ = std::move(positions);
    faces_ = std::move(faces);
    applied_ = positions_;
    history_.clear(); cursor_ = 0; history_bytes_ = 0;
  }
  void load(const pixel3d::Mesh& mesh) {
    std::vector<Face> faces;
    faces.reserve(mesh.tris.size());
    for (const auto& t : mesh.tris) faces.push_back({t[0], t[1], t[2]});
    load(mesh.positions, std::move(faces));
  }
  const auto& positions() const { return positions_; }
  const auto& faces() const { return faces_; }
  bool canUndo() const { return cursor_ != 0; }
  bool canRedo() const { return cursor_ < history_.size(); }
  // angle is radians; scale is positive and uniform. A selection is explicit:
  // coincident vertices in independent shells are never selected implicitly.
  bool transform(const std::set<uint32_t>& selected, Transform kind,
                 pixel3d::vec3d vector, double value = 0) {
    if (selected.empty()) return false;
    require(finite(vector) && std::isfinite(value), "Non-finite transform");
    require(kind != Transform::Scale || (value > 0 && value <= 100), "Scale must be positive and bounded");
    if (kind == Transform::Rotate) {
      const double length = pixel3d::length(vector);
      require(length > 1e-12, "Rotation axis is empty");
      vector = vector / length;
    }
    pixel3d::vec3d pivot{};
    for (auto i : selected) {
      require(i < positions_.size(), "Selection index out of range");
      pivot = pivot + positions_[i];
    }
    pivot = pivot / double(selected.size());
    Command command;
    auto candidate = positions_;
    for (auto i : selected) {
      auto p = positions_[i];
      if (kind == Transform::Move) p = p + vector;
      else if (kind == Transform::Scale) p = pivot + (p - pivot) * value;
      else {
        const auto q = p - pivot;
        p = pivot + q * std::cos(value) + pixel3d::cross(vector, q) * std::sin(value)
            + vector * (pixel3d::dot(vector, q) * (1 - std::cos(value)));
      }
      candidate[i] = p;
      command.changes.push_back({i, positions_[i], p});
    }
    validate(candidate, faces_); // Atomic rejection: failed edits never modify the document.
    const size_t bytes = command.changes.size() * sizeof(Change);
    require(bytes <= kHistoryBytes, "Selection exceeds bounded Undo memory");
    while (history_.size() > cursor_) {
      history_bytes_ -= history_.back().changes.size() * sizeof(Change);
      history_.pop_back();
    }
    while (!history_.empty() && (history_bytes_ + bytes > kHistoryBytes || history_.size() >= 64)) {
      history_bytes_ -= history_.front().changes.size() * sizeof(Change);
      history_.erase(history_.begin()); --cursor_;
    }
    positions_ = std::move(candidate);
    history_bytes_ += bytes;
    history_.push_back(std::move(command)); ++cursor_;
    return true;
  }
  bool undo() {
    if (!canUndo()) return false;
    for (const auto& c : history_[--cursor_].changes) positions_[c.index] = c.before;
    return true;
  }
  bool redo() {
    if (!canRedo()) return false;
    for (const auto& c : history_[cursor_++].changes) positions_[c.index] = c.after;
    return true;
  }
  void apply() { applied_ = positions_; history_.clear(); cursor_ = history_bytes_ = 0; }
  void revert() { positions_ = applied_; history_.clear(); cursor_ = history_bytes_ = 0; }
  pixel3d::Mesh triangulated() const {
    pixel3d::Mesh mesh; mesh.positions = positions_;
    for (const auto& f : faces_) {
      mesh.tris.push_back({f[0], f[1], f[2]});
      if (f.size() == 4) mesh.tris.push_back({f[0], f[2], f[3]});
    }
    return mesh;
  }
  size_t historyBytes() const { return history_bytes_; }

 private:
  static constexpr size_t kHistoryBytes = 16 * 1024 * 1024;
  struct Change { uint32_t index; pixel3d::vec3d before, after; };
  struct Command { std::vector<Change> changes; };
  static bool finite(pixel3d::vec3d p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  }
  static void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
  static void validate(const std::vector<pixel3d::vec3d>& p, const std::vector<Face>& faces) {
    require(!p.empty() && !faces.empty() && p.size() <= 200000 && faces.size() <= 400000,
            "Mesh exceeds the bounded editor profile");
    for (auto v : p) require(finite(v), "Non-finite position");
    for (const auto& f : faces) {
      require(f.size() == 3 || f.size() == 4, "Only triangles and quads are supported");
      require(std::set<uint32_t>(f.begin(), f.end()).size() == f.size(), "Repeated face index");
      for (auto i : f) require(i < p.size(), "Face index out of range");
      for (size_t j = 1; j + 1 < f.size(); ++j) {
        const auto u = p[f[j]] - p[f[0]], v = p[f[j+1]] - p[f[0]];
        const double area = pixel3d::length(pixel3d::cross(u, v));
        require(std::isfinite(area) && area > pixel3d::length(u) * pixel3d::length(v) * 1e-12,
                "Edit creates a degenerate or numerically unstable face");
      }
    }
    // This is an edit integrity check, NOT a self-intersection or game-readiness audit.
  }
  std::vector<pixel3d::vec3d> positions_, applied_;
  std::vector<Face> faces_;
  std::vector<Command> history_;
  size_t cursor_ = 0, history_bytes_ = 0;
};
} // namespace p3dgui
