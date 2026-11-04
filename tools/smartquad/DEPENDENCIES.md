# Smart Quad dependencies and scope

The existing Pixel3D C++/Qt application supervises a separate offline Blender
worker. Geometry is accepted only after topology, intersection, sampled fidelity,
same-camera appearance and independent GLB export checks. The drawing project
and input asset are never rewritten. The defective TRELLIS banana is a rejection
regression, not a remesher input. CGAL is not used.

AutoRemesher **1.2.0** is the default feature-aware remesher. The official Windows
bundle was checksum-verified and its command-line interface tested locally.
Install it with `tools/fetch_autoremesher.ps1`, preserving its DLLs and plugins.
The expected path is `.toolchain/autoremesher/1.2.0/autoremesher.exe`; an explicit
path or `PIXEL3D_AUTOREMESHER` may also be supplied. It supports target quad count,
sharp-edge angle, curvature adaptivity and anisotropy. The adapter inspects its
actual output: a zero exit status alone does not establish successful remeshing.
Source: https://github.com/huxingyi/autoremesher/releases/tag/1.2.0
Pinned official Windows ZIP SHA-256:
`f6184622cef84f0bcf032a0474df44e2076f4c10c159f1af0cb7230259af7476`.

**Licensing:** the exact AutoRemesher 1.2.0 source is MIT. Actual tagged dependencies
include MIT isotropicremesher/meshoptimizer/QtAwesome/QtWaitingSpinner, Eigen
primarily MPL-2.0, TBB Apache-2.0, Qt under its applicable LGPL/commercial license,
and FontAwesome OFL/CC-BY notices. Its old CoMISo acknowledgment is stale: CoMISo,
libigl and Geogram are absent from this tagged build. Keep dependency notices and
meet applicable source/relinking obligations before redistributing a backend
bundle; external-process invocation does not erase those obligations. This
implementation does not automatically redistribute the downloaded toolchain.
The tested official Windows ZIP contains binaries, DLLs and plugins; it does not
provide a complete license-notice package. Collect the matching notices and
required source/relinking materials before shipping it with an application.
Exact source license: https://github.com/huxingyi/autoremesher/blob/1.2.0/LICENSE
Actual build: https://github.com/huxingyi/autoremesher/blob/1.2.0/autoremesher.pro

Blender **4.2+** is installed separately and used headlessly for UV packing,
selected-to-active PBR/normal baking, rendering, editable BLEND output and glTF
import/export. It is GPL-licensed; this application does not link libblender or
bundle a Blender binary. Set `PIXEL3D_BLENDER` or choose its executable in Advanced.
Blender licensing: https://www.blender.org/about/license/
The worker can use Blender's proven QuadriFlow if explicitly configured without
AutoRemesher; this is reported as a different backend, not mislabeled.

The read-only native geometry auditor/BVH is reused from the accepted TRELLIS
investigation. Its original implementation is retained unchanged in that backend.
This application's copy uses asset-scale tolerances and rejects malformed input.
BVH attribution: `LICENSES/Trellis-tri-bvh-MIT.txt`. The independent GLB checker
reuses Pixel3D's already-vendored TinyGLTF, stb and nlohmann/json implementations.

Supported glTF Principled PBR inputs include base color, roughness, metallic,
emission color/strength and alpha. Changed smooth components receive new UVs and
selected-to-active bakes from the original high-detail surface. Constant inputs
retain their original values. Texture alpha is packed into the new base-color
image; the copied material retains its alpha mode and threshold. Tangent-space
normal baking includes the original normal-map strength and geometric detail;
the resulting normal map uses strength 1, avoiding a second strength multiplier.
glTF occlusion uses the source texture's red channel; the original white-to-AO
strength mix is retained and only its raw AO input is rebaked. Unsupported or
ambiguous AO graphs fail explicitly. Nearest texture filtering is propagated for
pixel-style inputs. UV islands use a 36-pixel gap at the selected texture size
with 16-pixel bake gutters. Corner colors and original UVs/materials remain on
geometry-retention outputs; remeshed color contributions are baked into albedo.

Multi-material components and intentional open surfaces retain their topology
through conservative triangle pairing, without closing boundaries or joining
components. Closed single-material components may use AutoRemesher, including
hard-surface inputs, but sampled surface/feature, orientation, Euler and topology
gates must pass. If safe remeshing cannot be established, original geometry is
retained as editable paired quads when the quad-fraction gate permits it. The
subsequent Blender DECIMATE collapse operates only on independent Game copies,
projects onto the original surface and rebakes detail. Open boundaries, material
seams and preserved sharp features are protected from that collapse stage. The
Quad Master is never simplified to satisfy the Game triangle target.

With High Fidelity and Preserve Shape enabled, the default sampled surface and
feature limit is 0.005 times the asset bounding-box diagonal. LOD0 uses the Game
result; independent source-derived LOD1 and LOD2 allow 0.01 and 0.02 respectively.
These are explicit sampled limits, not a formal Hausdorff bound. Without High
Fidelity the base limit is 0.01; disabling Preserve Shape doubles the base limit
up to 0.03. LOD limits remain capped at 0.02. Six identical camera views require
silhouette IoU of at least 0.97 and, when Preserve Materials is enabled, mean
absolute color error of at most 0.10. Every accepted LOD must actually reduce
exported triangles. Unmet budgets are reported rather than silently relaxing
quality constraints; a chain with redundant or unsafe LODs is rejected.

The supported processing profile is static assets, at most 100,000 imported
vertices / 200,000 triangles and 128 disconnected components. Unsupported rigs,
animations, morphs and material extensions fail explicitly. Ambiguous contacts
that cannot be distinguished from split UV seams also fail explicitly. Failed
outputs remain in staging and are never advertised as accepted assets. A safe
geometry-retention fallback is reported with any missed budget; it is not evidence
that remeshing or budget reduction succeeded on that asset.

The Windows application starts workers suspended, places them in a kill-on-close
Job Object with a 6 GiB aggregate commit ceiling, then resumes them. Before launch
it requires 2.5 GiB available physical RAM and 4 GiB free space at the output
location. Its one-second watchdog retains a 1 GiB free-RAM reserve and imposes a
20-minute wall-clock limit. The worker also checks at least 2 GiB disk space when
invoked directly. Blender/Cycles uses the CPU with two worker threads; CUDA and
HIP are disabled for the worker process. Cancellation terminates the worker tree,
and closing the dialog closes its Job Object. Accepted outputs are copied from
staging only after all requested levels pass their gates. Outputs include
the packed editable QuadMaster.blend, geometry-only QuadMaster.obj, Game.glb,
requested validated LODs, identical-camera comparisons and a numerical report.
