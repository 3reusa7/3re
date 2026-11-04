#pragma once
// Texture for a styled mesh (polished styles, step 6). The styled solid lies
// inside the crisp one, so each styled triangle takes the texture mapping of
// the nearest crisp triangle of the same part: a projected face is projected
// from the same view, an Edge Extend or Flat Palette face keeps its UV map
// (through the point's projection onto the crisp face's plane). Faces the
// styling did not move come out exactly as in crisp; a bevel or rounded strip
// takes the texture of the face it was cut from, so every surface gets exactly
// one source view (no colour averaging) and manual assignments carry over.
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/texture/projector.hpp"

namespace pixel3d {

TextureResult transfer_textures(const Mesh& styled, const Mesh& crisp, const TextureResult& crisp_tex, Dims d);

}  // namespace pixel3d
