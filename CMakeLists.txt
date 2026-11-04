cmake_minimum_required(VERSION 3.25)
# The release version lives in VERSION (e.g. 2.0.0-alpha); tools/package.ps1 builds the release from it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PIXEL3D_VERSION_FULL LIMIT_COUNT 1)
if(NOT PIXEL3D_VERSION_FULL MATCHES "^([0-9]+)[.]([0-9]+)[.]([0-9]+)(-[0-9A-Za-z.]+)?$")
  message(FATAL_ERROR "VERSION must hold a version like 1.1.0-alpha, not '${PIXEL3D_VERSION_FULL}'")
endif()
set(PIXEL3D_VERSION_NUMBERS "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(Pixel3D VERSION ${PIXEL3D_VERSION_NUMBERS} LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(PIXEL3D_ENABLE_SMARTQUAD "Enable the separate Phase 2 Smart Quad work" OFF)
set(PIXEL3D_BUILD_LABEL "" CACHE STRING "Optional isolated build identifier")

include(cmake/Warnings.cmake)
include(cmake/WindowsResources.cmake)
include(cmake/ThirdParty.cmake)
include(cmake/onnxruntime.cmake)

# Before every add_subdirectory: tests declared in tools/ (geometry audit,
# TRELLIS cleanup) were silently not registered when this came later.
enable_testing()
add_subdirectory(core)
if(EXISTS ${CMAKE_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/apps/CMakeLists.txt)
  add_subdirectory(apps)
endif()

add_subdirectory(tests)
