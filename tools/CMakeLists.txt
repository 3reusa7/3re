# Fixture ground truth + renderer (shared by the generator and the tests).
add_library(p3d_fixture_gt STATIC
  fixturegen/gt_model.cpp
  fixturegen/gt_render.cpp
  fixturegen/hand_maps.cpp
  fixturegen/lowpoly_character.cpp)
target_include_directories(p3d_fixture_gt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p3d_fixture_gt PUBLIC pixel3d-core)
pixel3d_warnings(p3d_fixture_gt)

add_executable(pixel3d-fixturegen fixturegen/main.cpp)
target_link_libraries(pixel3d-fixturegen PRIVATE p3d_fixture_gt)
pixel3d_executable(pixel3d-fixturegen)

# Independent GLB verifier: reloads an exported file with tinygltf in its own process.
add_executable(pixel3d-glbcheck glbcheck/main.cpp)
target_link_libraries(pixel3d-glbcheck PRIVATE pixel3d-core)
pixel3d_executable(pixel3d-glbcheck)
pixel3d_windows_resources(pixel3d-glbcheck "Pixel3D independent GLB checker")

# LOW-POLY STUDIO developer tool: builds fixture E at given budgets, prints the
# layout/allocation and writes review renders (turntables, test pose, sheets).
add_executable(pixel3d-lowpoly-dev lowpoly_dev/main.cpp)
target_link_libraries(pixel3d-lowpoly-dev PRIVATE pixel3d-core)
target_include_directories(pixel3d-lowpoly-dev PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(pixel3d-lowpoly-dev PRIVATE PIXEL3D_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
pixel3d_executable(pixel3d-lowpoly-dev)

# 1.2.0 P2 developer tool (not packaged): rebuilds the exact GLB that validation
# round-trips for a project and prints every sub-check (glb_roundtrip diagnosis).
add_executable(pixel3d-glbdiag glbdiag/main.cpp)
target_link_libraries(pixel3d-glbdiag PRIVATE pixel3d-core)
pixel3d_executable(pixel3d-glbdiag)

# Offline generated geometry validation, shared with future asset tools.
add_executable(pixel3d-geometry-audit smartquad/geometry_audit.cpp smartquad/tri_bvh.cpp)
target_include_directories(pixel3d-geometry-audit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/smartquad)
set_target_properties(pixel3d-geometry-audit PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)
pixel3d_executable(pixel3d-geometry-audit)
pixel3d_windows_resources(pixel3d-geometry-audit "Pixel3D read-only surface intersection audit")
add_test(NAME generated-geometry-audit COMMAND pixel3d-geometry-audit --self-test)
add_executable(pixel3d-exact-degenerate-tests ${CMAKE_SOURCE_DIR}/tests/gui/test_exact_degenerate.cpp)
target_include_directories(pixel3d-exact-degenerate-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME trellis-exact-degenerate COMMAND pixel3d-exact-degenerate-tests)

# Geometry-safety pieces of the TRELLIS cleanup (contact-guarded QEM, zero-length
# edge collapse, inner offset shells). Self-contained: no TRELLIS source needed.
find_package(OpenMP)
add_executable(pixel3d-trellis-geometry-safety-tests ${CMAKE_SOURCE_DIR}/tests/gui/test_trellis_geometry_safety.cpp
  trellis/decimate_qem_guarded.cpp smartquad/tri_bvh.cpp)
target_include_directories(pixel3d-trellis-geometry-safety-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/trellis ${CMAKE_CURRENT_SOURCE_DIR}/smartquad)
if(OpenMP_CXX_FOUND)
  target_link_libraries(pixel3d-trellis-geometry-safety-tests PRIVATE OpenMP::OpenMP_CXX)
endif()
add_test(NAME trellis-geometry-safety COMMAND pixel3d-trellis-geometry-safety-tests)

# Compile only the offline cleanup entry; reuse the pinned, verified DC/QEM
# archive without rebuilding the TRELLIS engine or altering its protected files.
set(PIXEL3D_TRELLIS_SOURCE_ROOT "" CACHE PATH "Existing verified TRELLIS backend source/archive")
if(PIXEL3D_TRELLIS_SOURCE_ROOT)
  add_executable(pixel3d-trellis-post-preview4 trellis/post_geometry_preview4.cpp)
  target_include_directories(pixel3d-trellis-post-preview4 PRIVATE "${PIXEL3D_TRELLIS_SOURCE_ROOT}/include")
  target_link_libraries(pixel3d-trellis-post-preview4 PRIVATE "${PIXEL3D_TRELLIS_SOURCE_ROOT}/build-cuda-prebuilt/libtrellis_core.a")
  if(MINGW)
    target_link_options(pixel3d-trellis-post-preview4 PRIVATE -fopenmp)
  endif()
  set_target_properties(pixel3d-trellis-post-preview4 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)

  # Same command line as Preview4; intersection-safe QEM, slit collapse and
  # inner-shell removal (tools/trellis/README.md, "Geometry safety").
  add_executable(pixel3d-trellis-post-safe trellis/post_geometry_safe.cpp trellis/decimate_qem_guarded.cpp)
  target_include_directories(pixel3d-trellis-post-safe PRIVATE "${PIXEL3D_TRELLIS_SOURCE_ROOT}/include"
    ${CMAKE_CURRENT_SOURCE_DIR}/trellis)
  target_link_libraries(pixel3d-trellis-post-safe PRIVATE "${PIXEL3D_TRELLIS_SOURCE_ROOT}/build-cuda-prebuilt/libtrellis_core.a")
  if(MINGW)
    # Runs from TrellisRuntime next to the engine, where no libgomp DLL ships.
    # FindOpenMP links the DLL import library, so use the flag directly.
    target_compile_options(pixel3d-trellis-post-safe PRIVATE -fopenmp)
    target_link_options(pixel3d-trellis-post-safe PRIVATE -fopenmp -static -static-libgcc -static-libstdc++)
  elseif(OpenMP_CXX_FOUND)
    target_link_libraries(pixel3d-trellis-post-safe PRIVATE OpenMP::OpenMP_CXX)
  endif()
  set_target_properties(pixel3d-trellis-post-safe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)
endif()
add_executable(pixel3d-asset-inspect asset_inspect/main.cpp)
target_link_libraries(pixel3d-asset-inspect PRIVATE p3d_generated_asset)
set_target_properties(pixel3d-asset-inspect PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)
pixel3d_executable(pixel3d-asset-inspect)
pixel3d_windows_resources(pixel3d-asset-inspect "Pixel3D generated mesh inspection")

if(PIXEL3D_ENABLE_SMARTQUAD)
# General-purpose imported-asset pipeline. Reuses the accepted read-only
# intersection auditor; this does not change TRELLIS extraction or QEM.
add_executable(pixel3d-smartquad-audit smartquad/geometry_audit.cpp smartquad/tri_bvh.cpp)
target_include_directories(pixel3d-smartquad-audit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/smartquad)
set_target_properties(pixel3d-smartquad-audit PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)
add_test(NAME smartquad-geometry-audit COMMAND pixel3d-smartquad-audit --self-test)

add_executable(pixel3d-smartquad-check smartquad/glb_check.cpp)
target_link_libraries(pixel3d-smartquad-check PRIVATE pixel3d-core)
set_target_properties(pixel3d-smartquad-check PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps)
if(MINGW)
  # Blender launches these outside the Qt development environment. Make the
  # independent helpers self-contained instead of relying on MinGW's PATH.
  target_link_options(pixel3d-smartquad-audit PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_options(pixel3d-smartquad-check PRIVATE -static -static-libgcc -static-libstdc++)
endif()
endif()
