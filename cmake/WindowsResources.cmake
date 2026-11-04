# Windows resources for Pixel3D's executables (spec M5 §2): VERSIONINFO and an
# application manifest (UTF-8 active code page, Windows 10/11, per-monitor-v2
# DPI awareness for GUI programs).
function(pixel3d_windows_resources target description)
  if(NOT WIN32)
    return()
  endif()
  cmake_parse_arguments(ARG "GUI" "" "" ${ARGN})
  set(PIXEL3D_RC_TARGET ${target})
  get_target_property(PIXEL3D_RC_OUTPUT ${target} OUTPUT_NAME)
  if(NOT PIXEL3D_RC_OUTPUT)
    set(PIXEL3D_RC_OUTPUT ${target})
  endif()
  set(PIXEL3D_RC_DESCRIPTION "${description}")
  set(PIXEL3D_RC_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/${target}.manifest)
  set(PIXEL3D_RC_DPI "")
  if(ARG_GUI)
    string(CONCAT PIXEL3D_RC_DPI
      "\n      <dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">true/pm</dpiAware>"
      "\n      <dpiAwareness xmlns=\"http://schemas.microsoft.com/SMI/2016/WindowsSettings\">PerMonitorV2</dpiAwareness>")
  endif()
  configure_file(${PROJECT_SOURCE_DIR}/cmake/pixel3d.manifest.in ${PIXEL3D_RC_MANIFEST} @ONLY)
  configure_file(${PROJECT_SOURCE_DIR}/cmake/pixel3d_version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc @ONLY)
  target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
  # The resource compiler does not see the manifest as a dependency on its own.
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc PROPERTIES OBJECT_DEPENDS ${PIXEL3D_RC_MANIFEST})
endfunction()
