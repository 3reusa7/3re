file(GLOB_RECURSE PIXEL3D_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixel3d/*.cpp)
file(GLOB_RECURSE PIXEL3D_CORE_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixel3d/*.hpp)

add_library(pixel3d-core STATIC ${PIXEL3D_CORE_SOURCES} ${PIXEL3D_CORE_HEADERS})
target_include_directories(pixel3d-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pixel3d-core PUBLIC manifold::manifold Clipper2::Clipper2 p3d_thirdparty_headers p3d_miniz)
target_link_libraries(pixel3d-core PRIVATE p3d_webp)
target_compile_definitions(pixel3d-core PUBLIC PIXEL3D_VERSION="${PIXEL3D_VERSION_FULL}")
pixel3d_warnings(pixel3d-core)
