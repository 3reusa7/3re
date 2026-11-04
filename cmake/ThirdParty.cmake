# Vendored dependencies (third_party/README.md lists versions and licences).
# Everything builds offline: FetchContent is disconnected and Clipper2 is redirected
# to the vendored copy at the commit Manifold pins.
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_CLIPPER2 ${CMAKE_SOURCE_DIR}/third_party/Clipper2 CACHE PATH "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
set(MANIFOLD_CROSS_SECTION ON CACHE BOOL "" FORCE)
set(MANIFOLD_USE_BUILTIN_CLIPPER2 ON CACHE BOOL "" FORCE)
set(MANIFOLD_DEBUG OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/manifold EXCLUDE_FROM_ALL)

add_library(p3d_thirdparty_headers INTERFACE)
target_include_directories(p3d_thirdparty_headers SYSTEM INTERFACE
  ${CMAKE_SOURCE_DIR}/third_party
  ${CMAKE_SOURCE_DIR}/third_party/stb
  ${CMAKE_SOURCE_DIR}/third_party/tinygltf)

add_library(p3d_miniz STATIC ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(p3d_miniz SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniz)

# Monocypher 4.0.3 (CC0-1.0 or BSD-2-Clause): Ed25519 + BLAKE2b for the signed update manifests (apps/update).
add_library(p3d_monocypher STATIC ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher.c
                                  ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher-ed25519.c)
target_include_directories(p3d_monocypher SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/monocypher)
set_target_properties(p3d_monocypher PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)

# libwebp 1.6.0, decode only (decoder, DSP for C/SSE2/SSE4.1 builds, decoder utils,
# demux for the first frame of animated files). third_party/libwebp/README.p3d.md lists
# what was kept and how the release was verified.
file(GLOB P3D_WEBP_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dec/*.c
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dsp/*.c
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/utils/*.c
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/demux/*.c)
add_library(p3d_webp STATIC ${P3D_WEBP_SOURCES})
target_include_directories(p3d_webp PRIVATE ${CMAKE_SOURCE_DIR}/third_party/libwebp)
target_include_directories(p3d_webp SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/libwebp/src)
set_target_properties(p3d_webp PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
