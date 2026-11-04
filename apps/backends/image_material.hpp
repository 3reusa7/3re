#pragma once
#include <QImage>
#include <QString>
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"
namespace p3dasset {
std::shared_ptr<const pixel3d::TexturedMesh> project_source_material(const pixel3d::Mesh& mesh,const QImage& image,int texture_size,bool crisp);
void save_source_material(const pixel3d::TexturedMesh& mesh,const QString& file);
}
