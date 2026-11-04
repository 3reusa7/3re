# Topology for game assets (v1.0.0)

* Engines draw triangles; a "game-ready" mesh is judged by triangle count, silhouette and shading, not by quads.
  3re's delivered meshes are triangle meshes (TRELLIS output, QEM-decimated). Quad retopology (Smart Retopology) is
  experimental and not part of the agent's delivery.
* Self-crossings: game engines render touching and crossing layers (cloth over body); for game targets the agent
  reports crossings and does not block on them (M2 ruling, `docs/agent/FIDELITY.md`). For 3D printing / booleans
  the spec uses `geometry.intersections = "block"`.
* Fidelity of a reduced mesh is measured against the original (distance p95/mean/max as % of the diagonal,
  silhouette IoU, normals, appearance), thresholds fixed before the M2 benchmark (`docs/agent/FIDELITY.md`).
* Open edges and non-manifold edges are measured by the geometry audit; the report lists them.

Sources: Khronos glTF 2.0 specification (meshes, primitives) https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html ;
meshoptimizer (MIT) simplifier documentation https://github.com/zeux/meshoptimizer ; our FIDELITY.md.
