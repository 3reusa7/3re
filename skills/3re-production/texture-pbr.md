# Texture and PBR (v1.0.0)

* 3re delivers one base-colour texture (glTF `pbrMetallicRoughness.baseColorTexture`, sRGB), 1024/2048/4096,
  baked from the pictures; metallic/roughness are constants. No normal, roughness or metallic maps are generated for
  TRELLIS models (glass gets alpha, M2.3).
* Where the colour comes from (provenance map next to every textured GLB): seen in a picture, blended at a picture's
  edge, reconstructed, fallback colour, TRELLIS native texture (sides no picture shows), extra picture (multi-angle).
* Hidden sides: without a picture of the back, the back is inferred (TRELLIS). The texture gate measures flat
  patches, greyness, foreign patches and front copies there (hybrid-texture 2.2; W2 of M4: the front-copy check is
  judged only where the back is mostly inferred).
* "Polished" in 3re is a measurement: sides painted from real pictures, budget, fidelity, texture and Godot PASS.
* Style transfer (cel-shading, "dark fantasy" repaint) is NOT implemented: a requested style is recorded, not applied.

Sources: Khronos glTF 2.0 specification, materials https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#materials ;
our reports docs/reports/{hybrid-texture,texture-v2,multi-angle}.
