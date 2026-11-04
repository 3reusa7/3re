// Isolated inspection process: bounded static geometry, no repair or inference.
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
  try {
    if (argc != 3) throw std::runtime_error("Usage: pixel3d-asset-inspect generated.glb new-output-directory");
    const auto out = pixel3d::path_from_utf8(argv[2]);
    if (std::filesystem::exists(out)) throw std::runtime_error("Refusing existing inspection directory");
    std::filesystem::create_directories(out);
    const auto mesh = p3dasset::load_geometry_glb(pixel3d::path_from_utf8(argv[1]));
    const auto report = p3dasset::inspect_geometry(mesh);
    p3dasset::write_geometry_binary(mesh, out / "geometry.bin");
    std::ofstream stream(out / "topology.json"); stream << report.dump(2) << '\n';
    if (!stream) throw std::runtime_error("Could not save inspection report");
    std::cout << report.dump() << '\n'; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n';return 1; }
}
