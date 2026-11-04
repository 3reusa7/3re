#pragma once
// Wavefront OBJ + MTL writer (the universal fallback; Roblox's importer reads
// it): one object, one material whose diffuse map is the PNG atlas, one
// v/vt/vn triple per mesh vertex, triangles. Coordinates go through the same
// export frame as glTF (origin, units, front). OBJ texture v points up, so
// vt = (u, 1 − v). OBJ/MTL cannot express NEAREST sampling.
#include <string>
#include <vector>

#include "pixel3d/io/glb_writer.hpp"

namespace pixel3d {

struct ObjFiles {
  std::string obj, mtl;     // the .mtl is referenced as <base>.mtl, its map_Kd as <base>.png
  std::vector<uint8_t> png;
};

ObjFiles write_obj(const TexturedMesh& m, const ExportOptions& opt, const std::string& base_name);

}  // namespace pixel3d
