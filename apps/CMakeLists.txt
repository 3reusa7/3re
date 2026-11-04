add_library(p3d_generated_asset STATIC backends/mesh_asset.cpp)
target_include_directories(p3d_generated_asset PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p3d_generated_asset PUBLIC pixel3d-core)
pixel3d_warnings(p3d_generated_asset)

add_library(p3d_cli_args STATIC cli/cli_args.cpp)
target_include_directories(p3d_cli_args PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p3d_cli_args PUBLIC pixel3d-core)
pixel3d_warnings(p3d_cli_args)

add_executable(pixel3d-cli cli/main.cpp)
target_link_libraries(pixel3d-cli PRIVATE p3d_cli_args)
set_target_properties(pixel3d-cli PROPERTIES OUTPUT_NAME 3re-cli)
pixel3d_executable(pixel3d-cli)
pixel3d_windows_resources(pixel3d-cli "3re command line")

# MCP server: protocol + tool catalog as a library (unit-tested in-process), thin exe.
set(PIXEL3D_SKILL_FILE ${CMAKE_SOURCE_DIR}/skills/pixel3d-reconstruct/SKILL.md)
file(READ ${PIXEL3D_SKILL_FILE} PIXEL3D_SKILL_MD)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PIXEL3D_SKILL_FILE})
configure_file(mcp/skill_text.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/mcp_skill_text.cpp @ONLY)

add_library(p3d_mcp STATIC
  mcp/args.cpp
  mcp/catalog.cpp
  mcp/server.cpp
  mcp/tools.cpp
  mcp/tools_edit.cpp
  mcp/tools_render.cpp
  mcp/tools_lowpoly.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/mcp_skill_text.cpp)
target_include_directories(p3d_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p3d_mcp PUBLIC pixel3d-core)
pixel3d_warnings(p3d_mcp)

add_executable(Pixel3D-mcp mcp/main.cpp)
target_link_libraries(Pixel3D-mcp PRIVATE p3d_mcp)
set_target_properties(Pixel3D-mcp PROPERTIES OUTPUT_NAME 3re-mcp)
pixel3d_executable(Pixel3D-mcp)
pixel3d_windows_resources(Pixel3D-mcp "3re MCP server (stdio)")

# Updates and first-run downloads (docs/UPDATES.md): a Qt-free core (minisign, versions, manifest, the
# folder swap) for 3re-update.exe and the tests, and the Qt service the studio uses.
add_library(p3d_update_core STATIC update/minisign.cpp update/version.cpp update/manifest.cpp update/swap.cpp)
target_include_directories(p3d_update_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(p3d_update_core PUBLIC p3d_thirdparty_headers PRIVATE p3d_monocypher)
pixel3d_warnings(p3d_update_core)
if(WIN32)
  add_executable(threere-update update/updater_main.cpp)
  set_target_properties(threere-update PROPERTIES OUTPUT_NAME 3re-update)
  target_link_options(threere-update PRIVATE -municode)
  target_link_libraries(threere-update PRIVATE p3d_update_core)
  pixel3d_executable(threere-update)
  pixel3d_windows_resources(threere-update "3re updater")
endif()
set(THREERE_UPDATE_REPOSITORY "3reusa7/3re" CACHE STRING "GitHub owner/name whose Releases 3re checks for updates")

# Desktop application (Qt 6). Built when Qt is available (see tools/bootstrap_toolchain.ps1).
find_package(Qt6 6.5 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets Concurrent Network)
if(Qt6_FOUND)
  add_library(p3d_update STATIC update/download.cpp update/update_service.cpp update/model_store.cpp)
  set_target_properties(p3d_update PROPERTIES AUTOMOC ON)
  target_link_libraries(p3d_update PUBLIC p3d_update_core pixel3d-core Qt6::Network Qt6::Concurrent PRIVATE p3d_miniz)
  target_compile_definitions(p3d_update PRIVATE THREERE_UPDATE_REPOSITORY="${THREERE_UPDATE_REPOSITORY}"
    THREERE_SOURCE_MODELS_CATALOG="${CMAKE_CURRENT_SOURCE_DIR}/update/models.json")
  pixel3d_warnings(p3d_update)
  if(TARGET threere-update)
    add_dependencies(p3d_update threere-update)
  endif()
  add_library(p3d_trellis_diagnostics STATIC backends/trellis_diagnostics.cpp)
  target_include_directories(p3d_trellis_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(p3d_trellis_diagnostics PUBLIC Qt6::Core)
  add_executable(pixel3d-cuda-health backends/cuda_health_main.cpp)
  target_link_libraries(pixel3d-cuda-health PRIVATE p3d_trellis_diagnostics)
  add_library(p3d_trellis_backend STATIC backends/trellis_backend.cpp backends/trellis_backend.hpp backends/trellis_native_post.cpp backends/retexture_batch.cpp backends/retexture_batch.hpp backends/image_input.cpp backends/image_input.hpp backends/image_material.cpp backends/image_material.hpp)
  set_target_properties(p3d_trellis_backend PROPERTIES AUTOMOC ON)
  target_link_libraries(p3d_trellis_backend PUBLIC p3d_generated_asset Qt6::Gui Qt6::Concurrent)
  target_link_libraries(p3d_trellis_backend PRIVATE p3d_webp PUBLIC p3d_trellis_diagnostics)
  if(WIN32)
    target_link_libraries(p3d_trellis_backend PRIVATE pdh psapi)
  endif()
  pixel3d_warnings(p3d_trellis_backend)
  # Segmentation (spec 3re §7): user-labelled regions, colour regions, SAM 2.1-tiny (ONNX Runtime, loaded at run time).
  add_library(p3d_segmentation STATIC
    segmentation/mask_set.cpp segmentation/mask_set.hpp
    segmentation/color_regions.cpp segmentation/color_regions.hpp
    segmentation/segmentation_store.cpp segmentation/segmentation_store.hpp
    segmentation/sam_engine.cpp segmentation/sam_engine.hpp)
  set_target_properties(p3d_segmentation PROPERTIES AUTOMOC ON)
  target_include_directories(p3d_segmentation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_include_directories(p3d_segmentation SYSTEM PRIVATE ${THREERE_ORT_INCLUDE})
  target_link_libraries(p3d_segmentation PUBLIC Qt6::Gui)
  if(WIN32)
    target_link_libraries(p3d_segmentation PRIVATE psapi)
  endif()
  pixel3d_warnings(p3d_segmentation)
  add_library(p3d_gui STATIC
    gui/app_controller.cpp
    gui/app_identity.cpp
    gui/self_test.cpp
    gui/shell/nav_rail.cpp
    gui/shell/step_bar.cpp
    gui/shell/spinner.cpp
    gui/shell/feature_badge.cpp
    gui/shell/coming_soon.cpp
    gui/dialogs.cpp
    gui/editor_state.cpp
    gui/history_panel.cpp
    gui/icons.cpp
    gui/inspector.cpp
    gui/issues.cpp
    gui/log_panel.cpp
    gui/lowpoly_panel.cpp
    gui/main_window.cpp
    gui/project/project_store.cpp
    gui/studio/studio_window.cpp
    gui/studio/home_page.cpp
    gui/studio/settings_page.cpp
    gui/studio/update_ui.cpp
    gui/studio/angles_panel.cpp
    gui/studio/rigging_page.cpp
    gui/studio/export_page.cpp
    gui/studio/image3d_workspace.cpp
    gui/studio/preview_bar.cpp
    gui/studio/splash.cpp
    gui/studio/e2e_driver.cpp
    gui/studio/retopo_mesh_editor.cpp
    gui/studio/retopo_workspace.cpp
    gui/studio/agent_run_model.cpp
    gui/studio/agent_page.cpp
    gui/studio/agent_chat_panel.cpp
    segmentation/segment_editor.cpp
    gui/start_screen.cpp
    gui/theme.cpp
    gui/view_state.cpp
    gui/viewport.cpp
    gui/viewport_gl.cpp
    gui/views_panel.cpp
    gui/widgets.cpp
    # Headers listed so AUTOMOC finds every Q_OBJECT.
    gui/app_controller.hpp
    gui/dialogs.hpp
    gui/editor_state.hpp
    gui/history_panel.hpp
    gui/inspector.hpp
    gui/log_panel.hpp
    gui/lowpoly_panel.hpp
    gui/main_window.hpp
    gui/studio/studio_window.hpp
    gui/studio/home_page.hpp
    gui/studio/settings_page.hpp
    gui/studio/update_ui.hpp
    gui/studio/rigging_page.hpp
    gui/studio/export_page.hpp
    gui/studio/image3d_workspace.hpp
    gui/studio/preview_bar.hpp
    gui/studio/splash.hpp
    gui/studio/e2e_driver.hpp
    gui/studio/retopo_mesh_editor.hpp
    gui/studio/retopo_workspace.hpp
    gui/studio/agent_page.hpp
    gui/studio/agent_chat_panel.hpp
    segmentation/segment_editor.hpp
    gui/shell/spinner.hpp
    gui/start_screen.hpp
    gui/viewport.hpp
    gui/viewport_gl.hpp
    gui/views_panel.hpp
    gui/widgets.hpp
    gui/shell/nav_rail.hpp
    gui/shell/step_bar.hpp
    gui/shell/spinner.hpp
    gui/shell/coming_soon.hpp)
  target_include_directories(p3d_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/gui)
  target_link_libraries(p3d_gui PUBLIC pixel3d-core p3d_trellis_backend p3d_segmentation p3d_agent_chat p3d_update Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
  if(PIXEL3D_ENABLE_SMARTQUAD)
    target_sources(p3d_gui PRIVATE gui/smart_quad_dialog.cpp gui/smart_quad_dialog.hpp)
    target_compile_definitions(p3d_gui PUBLIC PIXEL3D_ENABLE_SMARTQUAD=1)
  endif()
  set_target_properties(p3d_gui PROPERTIES AUTOMOC ON)
  pixel3d_warnings(p3d_gui)

  # Dynamic Qt and MinGW runtime, deployed beside the exe (windeployqt in tools/package.ps1).
  # Icon: tools/make_3re_icon.py generates resources/3re.{png,ico}.
  set(PIXEL3D_GUI_RESOURCES gui/resources/3re.qrc)
  if(WIN32)
    list(APPEND PIXEL3D_GUI_RESOURCES gui/resources/3re.rc)
  endif()
  add_executable(Pixel3D WIN32 gui/main.cpp ${PIXEL3D_GUI_RESOURCES})
  target_link_libraries(Pixel3D PRIVATE p3d_gui)
  if(PIXEL3D_BUILD_LABEL)
    target_compile_definitions(Pixel3D PRIVATE PIXEL3D_BUILD_LABEL="${PIXEL3D_BUILD_LABEL}")
  endif()
  set_target_properties(Pixel3D PROPERTIES AUTOMOC ON AUTORCC ON OUTPUT_NAME 3re-studio)
  pixel3d_warnings(Pixel3D)
  pixel3d_windows_resources(Pixel3D "3re 3D creation studio" GUI)
  add_dependencies(Pixel3D pixel3d-asset-inspect pixel3d-geometry-audit pixel3d-cuda-health)
  if(PIXEL3D_ENABLE_SMARTQUAD)
  # Phase 2 remains opt-in; its existing implementation is preserved.
  add_custom_target(pixel3d-smartquad-worker
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Pixel3D>/smartquad"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/smartquad/pipeline.py" "$<TARGET_FILE_DIR:Pixel3D>/smartquad/pipeline.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/smartquad/game_simplify.py" "$<TARGET_FILE_DIR:Pixel3D>/smartquad/game_simplify.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tools/smartquad/quadwild_adapter.py" "$<TARGET_FILE_DIR:Pixel3D>/smartquad/quadwild_adapter.py"
    COMMENT "Copying Smart Quad worker scripts")
  add_dependencies(Pixel3D pixel3d-smartquad-worker)
  if(TARGET pixel3d-smartquad-audit AND TARGET pixel3d-smartquad-check)
    add_dependencies(Pixel3D pixel3d-smartquad-audit pixel3d-smartquad-check)
  endif()
  endif()
else()
  message(STATUS "Qt 6 not found: the Pixel3D desktop application is not built")
endif()

# Saved-atlas source appearance experiment; not routed into generation by default.
if(Qt6_FOUND)
  add_library(p3d_texture_atlas STATIC ../third_party/xatlas/xatlas.cpp)
  target_compile_definitions(p3d_texture_atlas PRIVATE XA_MULTITHREADED=0)
  target_include_directories(p3d_texture_atlas PUBLIC ../third_party/xatlas)
  add_library(p3d_visibility_texture STATIC backends/texture_visibility_engine.cpp backends/texture_atlas.cpp)
  target_link_libraries(p3d_visibility_texture PRIVATE p3d_texture_atlas)
  target_link_libraries(p3d_visibility_texture PUBLIC p3d_generated_asset Qt6::Gui)
  add_executable(pixel3d-texture-visible backends/texture_visibility_cli.cpp)
  target_link_libraries(pixel3d-texture-visible PRIVATE p3d_visibility_texture p3d_trellis_backend)

  # 3re Auto Texture (spec 2026-10-20-auto-texture-design): Qt-free engine + the worker the backend runs
  # after GeometrySafe and the geometry audit, and for retexturing saved GLBs.
  add_library(p3d_auto_texture STATIC backends/auto_texture_common.cpp backends/auto_texture_register.cpp
    backends/auto_texture_bake.cpp backends/auto_texture_charts.cpp backends/auto_texture_quality.cpp backends/auto_texture_symmetry.cpp
    backends/auto_texture_native.cpp backends/auto_texture_gate.cpp backends/auto_texture_multiview.cpp backends/auto_texture_views.cpp)
  # Own xatlas build with its task scheduler on (charting a 150k-triangle mesh is the slow stage).
  add_library(p3d_auto_xatlas STATIC ../third_party/xatlas/xatlas.cpp)
  target_include_directories(p3d_auto_xatlas PUBLIC ../third_party/xatlas)
  target_link_libraries(p3d_auto_texture PUBLIC p3d_generated_asset PRIVATE p3d_auto_xatlas)
  pixel3d_warnings(p3d_auto_texture)
  add_executable(pixel3d-auto-texture backends/auto_texture_cli.cpp)
  target_link_libraries(pixel3d-auto-texture PRIVATE p3d_auto_texture)
  if(WIN32)
    target_link_libraries(pixel3d-auto-texture PRIVATE psapi shell32)
  endif()
  pixel3d_executable(pixel3d-auto-texture)
  pixel3d_windows_resources(pixel3d-auto-texture "3re Auto Texture worker")
  add_dependencies(Pixel3D pixel3d-auto-texture)
  # Milestone 4 (W4): "there is a problem here" — a marked region's provenance and bounded regional fixes.
  add_executable(pixel3d-regional-fix backends/regional_fix_cli.cpp)
  target_link_libraries(pixel3d-regional-fix PRIVATE p3d_auto_texture)
  if(WIN32)
    target_link_libraries(pixel3d-regional-fix PRIVATE shell32)
  endif()
  pixel3d_executable(pixel3d-regional-fix)
  pixel3d_windows_resources(pixel3d-regional-fix "3re regional fix worker")
  add_dependencies(Pixel3D pixel3d-regional-fix)
  # Developer tool (not packaged): close-up renders and texel-density measurements of a textured GLB.
  add_executable(pixel3d-texture-inspect backends/texture_inspect_cli.cpp)
  target_link_libraries(pixel3d-texture-inspect PRIVATE p3d_auto_texture)
  if(WIN32)
    target_link_libraries(pixel3d-texture-inspect PRIVATE shell32)
  endif()
  pixel3d_executable(pixel3d-texture-inspect)

  # 3re Smart Retopology: the native side of the QuadWild-BiMDF engine (input, measurement, colour
  # transfer, GLB) and the worker the Retopology workspace runs.
  add_library(p3d_retopo STATIC backends/retopo_engine.cpp)
  target_link_libraries(p3d_retopo PUBLIC p3d_auto_texture)
  pixel3d_warnings(p3d_retopo)
  add_executable(pixel3d-retopo backends/retopo_cli.cpp)
  target_link_libraries(pixel3d-retopo PRIVATE p3d_retopo)
  if(WIN32)
    target_link_libraries(pixel3d-retopo PRIVATE psapi shell32)
  endif()
  pixel3d_executable(pixel3d-retopo)
  pixel3d_windows_resources(pixel3d-retopo "3re Smart Retopology worker")
  add_dependencies(Pixel3D pixel3d-retopo)
  # The workspace reads the worker's OBJ results and reuses the same OBJ reader.
  target_link_libraries(p3d_gui PUBLIC p3d_retopo)
endif()

# 3re.exe: the only file a user starts. A tiny native launcher (no Qt, static runtime)
# for app\3re-studio.exe, so the release folder's root stays clean (spec §3).
if(WIN32)
  add_executable(threere-launcher WIN32 launcher/launcher.cpp gui/resources/3re.rc)
  set_target_properties(threere-launcher PROPERTIES OUTPUT_NAME 3re)
  target_link_options(threere-launcher PRIVATE -municode)
  pixel3d_executable(threere-launcher)
  pixel3d_windows_resources(threere-launcher "3re" GUI)
endif()

# Game-Ready Asset Agent (docs/agent/PLAN.md): deterministic planner, executor, recovery and delivery
# on top of the existing workers. The core is Qt-free; 3re-agent.exe is a console program.
if(WIN32)
  add_library(p3d_agent_core STATIC agent/agent_spec.cpp agent/agent_plan.cpp agent/agent_recovery.cpp
    agent/agent_glb.cpp agent/agent_eval.cpp agent/agent_exec.cpp agent/agent_probe.cpp agent/agent_fidelity.cpp agent/agent_select.cpp
    agent/agent_gltf_decimate.cpp agent/agent_decide.cpp agent/agent_hygiene.cpp agent/agent_godot.cpp ../tools/smartquad/tri_bvh.cpp ../third_party/meshoptimizer/simplifier.cpp)
  target_include_directories(p3d_agent_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ../tools/smartquad ../third_party/meshoptimizer)
  target_link_libraries(p3d_agent_core PUBLIC pixel3d-core PRIVATE bcrypt)
  pixel3d_warnings(p3d_agent_core)

  # Milestone 4 chat agent (docs/agent/PLAN-M4.md): brain providers, tools, conversation log. Qt-free.
  add_library(p3d_agent_chat STATIC agent/chat/chat_words.cpp agent/chat/chat_rules.cpp agent/chat/chat_tools.cpp agent/chat/chat_session.cpp agent/chat/chat_http.cpp)
  target_include_directories(p3d_agent_chat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/agent/chat)
  target_link_libraries(p3d_agent_chat PUBLIC p3d_agent_core PRIVATE winhttp)
  pixel3d_warnings(p3d_agent_chat)
  add_executable(threere-chat agent/chat/chat_main.cpp agent/chat/chat_eval.cpp)
  set_target_properties(threere-chat PROPERTIES OUTPUT_NAME 3re-chat)
  target_link_libraries(threere-chat PRIVATE p3d_agent_chat)
  target_link_options(threere-chat PRIVATE -municode)
  pixel3d_executable(threere-chat)
  pixel3d_windows_resources(threere-chat "3re chat agent (console)")

  # The agent itself: console program (live log), runs the workers as separate processes.
  add_executable(threere-agent agent/agent_main.cpp agent/agent_run.cpp)
  set_target_properties(threere-agent PROPERTIES OUTPUT_NAME 3re-agent)
  target_link_libraries(threere-agent PRIVATE p3d_agent_core p3d_generated_asset)
  target_link_options(threere-agent PRIVATE -municode)
  pixel3d_executable(threere-agent)
  pixel3d_windows_resources(threere-agent "3re Game-Ready Asset Agent")

  # Guarded QEM (the shipped cleanup decimator, exact contact guard) on a GLB, for triangle budgets.
  add_executable(pixel3d-decimate-guarded ../tools/trellis/decimate_cli.cpp ../tools/trellis/decimate_qem_guarded.cpp)
  target_include_directories(pixel3d-decimate-guarded PRIVATE ../tools/trellis ../tools/smartquad)
  target_link_libraries(pixel3d-decimate-guarded PRIVATE p3d_agent_core p3d_generated_asset)
  target_link_options(pixel3d-decimate-guarded PRIVATE -municode)
  # As pixel3d-trellis-post-safe: OpenMP linked statically (no libgomp DLL ships).
  target_compile_options(pixel3d-decimate-guarded PRIVATE -fopenmp)
  target_link_options(pixel3d-decimate-guarded PRIVATE -fopenmp)
  pixel3d_executable(pixel3d-decimate-guarded)
  pixel3d_windows_resources(pixel3d-decimate-guarded "3re guarded decimation (asset agent)")

  # UV-preserving multi-mesh decimation of foreign GLBs (agent milestone 2, A3), materials untouched.
  add_executable(pixel3d-decimate-gltf agent/decimate_gltf_main.cpp)
  target_link_libraries(pixel3d-decimate-gltf PRIVATE p3d_agent_core)
  target_link_options(pixel3d-decimate-gltf PRIVATE -municode)
  pixel3d_executable(pixel3d-decimate-gltf)
  pixel3d_windows_resources(pixel3d-decimate-gltf "3re multi-mesh glTF decimation (asset agent)")

  # Game fidelity of a reduced mesh against its original (agent milestone 2, A1; docs/agent/FIDELITY.md).
  add_executable(pixel3d-mesh-fidelity agent/fidelity_main.cpp)
  target_link_libraries(pixel3d-mesh-fidelity PRIVATE p3d_agent_core)
  target_link_options(pixel3d-mesh-fidelity PRIVATE -municode)
  pixel3d_executable(pixel3d-mesh-fidelity)
  pixel3d_windows_resources(pixel3d-mesh-fidelity "3re mesh fidelity check (asset agent)")

  # Asset quality G0-G4 (docs/agent/ASSET-QUALITY.md): transform hygiene, detail and part tools.
  add_executable(pixel3d-asset-quality agent/asset_quality_main.cpp)
  target_link_libraries(pixel3d-asset-quality PRIVATE p3d_agent_core)
  target_link_options(pixel3d-asset-quality PRIVATE -municode)
  pixel3d_executable(pixel3d-asset-quality)
  pixel3d_windows_resources(pixel3d-asset-quality "3re asset quality checks (asset agent)")
endif()
