# What 3re can and cannot do (v1.0.0, 2.9.0-alpha)

Can (measured in the M2–M4 reports):
* One picture → 3D model (TRELLIS.2 Q4 on a 4 GB GPU, ~6–13 min, needs 3.25 GiB free RAM to start); or reuse a saved
  generation of the same picture; or start from a GLB.
* Reduce to a triangle budget within fidelity limits, or ask when it cannot (budget vs needed count).
* Texture from the front picture + up to 5 more pictures (back, sides, three-quarters, a split turnaround sheet);
  each extra picture is registered on the mesh and rejected when its angle label is wrong or mirrored.
* Check: geometry audit, texture gate, fidelity, Khronos validator, independent reload, Godot import, scale.
* Ask a person: budget decisions, weak hidden sides (asks for a back picture).
* "There is a problem here": diagnose a marked region (what painted it, whether a picture sees it) and try up to 3
  regional fixes from other textures of the same mesh, changing nothing outside the region (measured).

Cannot (say so plainly):
* Rigging / skinning / animation of generated characters (Rigging works only for Pixel Art Builder models).
* Style transfer (recorded only), PBR maps beyond base colour, LODs, UV unwrapping by hand, quad retopology delivery.
* Verify Unity or Unreal imports (Godot only).
* Read pictures in the rules brain (labels from file names; the registration verifies angles).
