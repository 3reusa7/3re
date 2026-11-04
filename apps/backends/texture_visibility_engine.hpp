#pragma once
#include <QImage>
#include <nlohmann/json.hpp>
#include "pixel3d/render/rasterizer.hpp"
namespace p3dasset {
struct VisibilityBake { pixel3d::TexturedMesh mesh; nlohmann::json evidence; QImage provenance; };
pixel3d::TexturedMesh create_source_atlas(const pixel3d::Mesh&, int size);
// Existing atlas and logical topology are mandatory. No geometry inference,
// remeshing, welding, material prediction or change to original input files.
VisibilityBake bake_visible_source(const pixel3d::Mesh& logical,
                                  const pixel3d::TexturedMesh& atlas,
                                  const QImage& source, int size, bool crisp);
void save_visibility_bake(const VisibilityBake&, const pixel3d::Mesh&, const QString&);
}
