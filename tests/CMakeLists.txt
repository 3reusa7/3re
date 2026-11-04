file(GLOB PIXEL3D_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(pixel3d-tests ${PIXEL3D_TEST_SOURCES})
target_link_libraries(pixel3d-tests PRIVATE pixel3d-core)
foreach(lib p3d_fixture_gt p3d_cli_args p3d_mcp)
  if(TARGET ${lib})
    target_link_libraries(pixel3d-tests PRIVATE ${lib})
  endif()
endforeach()
target_compile_definitions(pixel3d-tests PRIVATE
  PIXEL3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
pixel3d_executable(pixel3d-tests)

add_test(NAME pixel3d-tests COMMAND pixel3d-tests)

# End-to-end MCP run: a stdlib-only Python MCP client drives Pixel3D-mcp over stdio.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND TARGET Pixel3D-mcp AND TARGET pixel3d-glbcheck)
  add_test(NAME mcp-e2e
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/mcp/e2e_client.py
            $<TARGET_FILE:Pixel3D-mcp> ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
            ${CMAKE_CURRENT_BINARY_DIR}/mcp_e2e $<TARGET_FILE:pixel3d-glbcheck>)
endif()

# Desktop application tests (Qt Test, offscreen).
add_executable(threere-retopo-document-tests retopo/edit_document.cpp)
target_include_directories(threere-retopo-document-tests PRIVATE ${PROJECT_SOURCE_DIR}/apps ${PROJECT_SOURCE_DIR}/core)
target_link_libraries(threere-retopo-document-tests PRIVATE pixel3d-core)
pixel3d_warnings(threere-retopo-document-tests)
add_test(NAME retopo-edit-document COMMAND threere-retopo-document-tests)
if(TARGET p3d_retopo)
  # Smart Retopology engine side (no QuadWild needed): OBJ, closest points, measurement, colour transfer.
  add_executable(threere-retopo-engine-tests retopo/engine.cpp)
  target_link_libraries(threere-retopo-engine-tests PRIVATE p3d_retopo)
  pixel3d_warnings(threere-retopo-engine-tests)
  add_test(NAME retopo-engine COMMAND threere-retopo-engine-tests)
endif()
if(TARGET p3d_gui)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(pixel3d-generated-material-tests gui/test_generated_material.cpp)
  target_link_libraries(pixel3d-generated-material-tests PRIVATE p3d_generated_asset Qt6::Test)
  set_target_properties(pixel3d-generated-material-tests PROPERTIES AUTOMOC ON)
  add_test(NAME generated-material COMMAND pixel3d-generated-material-tests)
  set_tests_properties(generated-material PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    add_executable(pixel3d-trellis-upgrade-tests gui/test_trellis_upgrade.cpp)
    target_link_libraries(pixel3d-trellis-upgrade-tests PRIVATE p3d_gui Qt6::Test)
    set_target_properties(pixel3d-trellis-upgrade-tests PROPERTIES AUTOMOC ON)
    add_dependencies(pixel3d-trellis-upgrade-tests pixel3d-asset-inspect pixel3d-geometry-audit)
    add_test(NAME trellis-upgrade COMMAND pixel3d-trellis-upgrade-tests)
    set_tests_properties(trellis-upgrade PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
  add_executable(pixel3d-image3d-tests gui/test_image3d.cpp)
  target_link_libraries(pixel3d-image3d-tests PRIVATE p3d_gui Qt6::Test)
  set_target_properties(pixel3d-image3d-tests PROPERTIES AUTOMOC ON)
  add_dependencies(pixel3d-image3d-tests pixel3d-asset-inspect pixel3d-geometry-audit pixel3d-auto-texture)
  add_custom_command(TARGET pixel3d-image3d-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-auto-texture>" "$<TARGET_FILE_DIR:pixel3d-image3d-tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-asset-inspect>" "$<TARGET_FILE_DIR:pixel3d-image3d-tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-geometry-audit>" "$<TARGET_FILE_DIR:pixel3d-image3d-tests>")
  pixel3d_warnings(pixel3d-image3d-tests)
  add_test(NAME image3d-integration COMMAND pixel3d-image3d-tests)
  set_tests_properties(image3d-integration PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
  add_executable(pixel3d-trellis-workflow-smoke gui/trellis_workflow_smoke.cpp)
  target_link_libraries(pixel3d-trellis-workflow-smoke PRIVATE p3d_gui)
  # The dialog looks for every helper next to its own executable; without the
  # CUDA probe the GPU check fails and Generate never enables.
  add_dependencies(pixel3d-trellis-workflow-smoke pixel3d-asset-inspect pixel3d-geometry-audit pixel3d-cuda-health)
  add_custom_command(TARGET pixel3d-trellis-workflow-smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-asset-inspect>" "$<TARGET_FILE_DIR:pixel3d-trellis-workflow-smoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-geometry-audit>" "$<TARGET_FILE_DIR:pixel3d-trellis-workflow-smoke>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pixel3d-cuda-health>" "$<TARGET_FILE_DIR:pixel3d-trellis-workflow-smoke>")
  pixel3d_warnings(pixel3d-trellis-workflow-smoke)
  add_executable(pixel3d-gui-tests gui/test_gui.cpp)
  target_link_libraries(pixel3d-gui-tests PRIVATE p3d_gui Qt6::Test)
  set_target_properties(pixel3d-gui-tests PROPERTIES AUTOMOC ON)
  target_compile_definitions(pixel3d-gui-tests PRIVATE PIXEL3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
  pixel3d_warnings(pixel3d-gui-tests)
  add_test(NAME gui-tests COMMAND pixel3d-gui-tests)
  set_tests_properties(gui-tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
  if(PIXEL3D_ENABLE_SMARTQUAD)
  add_executable(pixel3d-smartquad-gui-tests gui/test_smart_quad.cpp)
  target_link_libraries(pixel3d-smartquad-gui-tests PRIVATE p3d_gui Qt6::Test)
  set_target_properties(pixel3d-smartquad-gui-tests PROPERTIES AUTOMOC ON)
  if(TARGET pixel3d-smartquad-audit AND TARGET pixel3d-smartquad-check)
    add_dependencies(pixel3d-smartquad-gui-tests pixel3d-smartquad-audit pixel3d-smartquad-check)
    add_custom_command(TARGET pixel3d-smartquad-gui-tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:pixel3d-smartquad-gui-tests>/smartquad"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tools/smartquad/pipeline.py"
        "$<TARGET_FILE_DIR:pixel3d-smartquad-gui-tests>/smartquad/pipeline.py"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:pixel3d-smartquad-audit>" "$<TARGET_FILE_DIR:pixel3d-smartquad-gui-tests>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:pixel3d-smartquad-check>" "$<TARGET_FILE_DIR:pixel3d-smartquad-gui-tests>")
  endif()
  pixel3d_warnings(pixel3d-smartquad-gui-tests)
  add_test(NAME smartquad-gui-tests COMMAND pixel3d-smartquad-gui-tests)
  set_tests_properties(smartquad-gui-tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
  endif()
endif()

# Independent glTF check for LOW-POLY STUDIO exports: the Khronos glTF-Validator
# (tools/fetch_gltf_validator.ps1 puts the pinned build in .toolchain/).
find_program(PIXEL3D_GLTF_VALIDATOR gltf_validator
  PATHS ${CMAKE_SOURCE_DIR}/.toolchain/gltf_validator/2.0.0-dev.3.10 NO_DEFAULT_PATH)
if(PIXEL3D_GLTF_VALIDATOR)
  target_compile_definitions(pixel3d-tests PRIVATE PIXEL3D_GLTF_VALIDATOR="${PIXEL3D_GLTF_VALIDATOR}")
else()
  message(WARNING "glTF-Validator not found: run tools/fetch_gltf_validator.ps1 (the skinned-export validation test will fail)")
endif()
target_compile_definitions(pixel3d-tests PRIVATE PIXEL3D_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/lowpoly_out")

if(TARGET p3d_gui)
  add_executable(pixel3d-trellis-diagnostics-tests gui/test_trellis_diagnostics.cpp)
  target_link_libraries(pixel3d-trellis-diagnostics-tests PRIVATE p3d_gui Qt6::Test)
  set_target_properties(pixel3d-trellis-diagnostics-tests PROPERTIES AUTOMOC ON)
  add_test(NAME trellis-diagnostics COMMAND pixel3d-trellis-diagnostics-tests)
  set_tests_properties(trellis-diagnostics PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()
if(TARGET p3d_visibility_texture)
  add_executable(pixel3d-visibility-tests gui/texture_visibility_tests.cpp)
  target_include_directories(pixel3d-visibility-tests PRIVATE ${PROJECT_SOURCE_DIR}/apps/backends)
  target_link_libraries(pixel3d-visibility-tests PRIVATE p3d_visibility_texture)
  add_test(NAME visibility-texture COMMAND pixel3d-visibility-tests)
  set_tests_properties(visibility-texture PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()

if(TARGET p3d_auto_texture)
  add_executable(pixel3d-auto-texture-tests gui/auto_texture_tests.cpp)
  target_include_directories(pixel3d-auto-texture-tests PRIVATE ${PROJECT_SOURCE_DIR}/apps/backends)
  target_link_libraries(pixel3d-auto-texture-tests PRIVATE p3d_auto_texture)
  add_test(NAME auto-texture COMMAND pixel3d-auto-texture-tests)
endif()

# 3re studio tests: one Qt Test executable, every test class self-registers (tests/studio/registry.hpp).
if(TARGET p3d_gui)
  file(GLOB THREERE_STUDIO_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/studio/*.cpp)
  add_executable(threere-studio-tests ${THREERE_STUDIO_TEST_SOURCES})
  target_link_libraries(threere-studio-tests PRIVATE p3d_gui Qt6::Test)
  set_target_properties(threere-studio-tests PROPERTIES AUTOMOC ON)
  target_compile_definitions(threere-studio-tests PRIVATE PIXEL3D_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    THREERE_SAM_DIR="${THREERE_SAM_DIR}" THREERE_ORT_DLL="${THREERE_ORT_DLL}")
  pixel3d_warnings(threere-studio-tests)
  add_test(NAME studio-tests COMMAND threere-studio-tests)
  set_tests_properties(studio-tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
endif()

# Updates and first-run downloads (apps/update, docs/UPDATES.md): the Qt-free core with 3re-update.exe, and
# the whole flow against a local server (update from the previous release, bad signature, corrupted
# download, roll-back, model downloads).
if(WIN32 AND TARGET threere-update)
  add_executable(threere-fake-release update/fake_release_app.cpp)
  pixel3d_executable(threere-fake-release)
  add_executable(threere-update-core-tests update/test_update_core.cpp)
  target_link_libraries(threere-update-core-tests PRIVATE p3d_update_core p3d_monocypher)
  target_compile_definitions(threere-update-core-tests PRIVATE UPDATER_EXE="$<TARGET_FILE:threere-update>"
    FAKE_RELEASE_APP="$<TARGET_FILE:threere-fake-release>" THREERE_UPDATE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/update/fixtures"
    THREERE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  pixel3d_executable(threere-update-core-tests)
  add_dependencies(threere-update-core-tests threere-update threere-fake-release)
  add_test(NAME update-core COMMAND threere-update-core-tests)
  if(TARGET p3d_update)
    add_executable(threere-update-flow-tests update/test_update_flow.cpp)
    target_link_libraries(threere-update-flow-tests PRIVATE p3d_update p3d_miniz Qt6::Test)
    set_target_properties(threere-update-flow-tests PROPERTIES AUTOMOC ON)
    target_compile_definitions(threere-update-flow-tests PRIVATE UPDATER_EXE="$<TARGET_FILE:threere-update>"
      FAKE_RELEASE_APP="$<TARGET_FILE:threere-fake-release>")
    pixel3d_warnings(threere-update-flow-tests)
    add_dependencies(threere-update-flow-tests threere-update threere-fake-release)
    add_test(NAME update-flow COMMAND threere-update-flow-tests)
    set_tests_properties(update-flow PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
  endif()
endif()

# 3re.exe launcher: forwards the command line and the exit code to app\3re-studio.exe.
if(WIN32 AND TARGET threere-launcher)
  add_executable(threere-echo-target launcher/echo_target.cpp)
  pixel3d_executable(threere-echo-target)
  foreach(case forwards missing)
    add_test(NAME launcher-${case} COMMAND ${CMAKE_COMMAND}
      -DLAUNCHER=$<TARGET_FILE:threere-launcher> -DTARGET=$<TARGET_FILE:threere-echo-target>
      -DOUT=${CMAKE_CURRENT_BINARY_DIR}/launcher_echo.txt -DCASE=${case}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/launcher/run_launcher_test.cmake)
  endforeach()
endif()

# Game-Ready Asset Agent core (docs/agent/PLAN.md): spec, planner, recovery, GLB, acceptance, executor, probe.
if(WIN32)
  add_executable(threere-agent-child agent/agent_child.cpp)
  pixel3d_executable(threere-agent-child)
  # Optional real Godot import check (skipped with a message when Godot is not there).
  file(TO_CMAKE_PATH "$ENV{USERPROFILE}/Desktop/Godot_v4.7-stable_win64_console.exe" THREERE_TEST_GODOT)
  add_executable(threere-agent-tests agent/test_agent.cpp)
  target_include_directories(threere-agent-tests PRIVATE ${CMAKE_SOURCE_DIR}/apps)
  if(TARGET p3d_agent_core)
    target_link_libraries(threere-agent-tests PRIVATE p3d_agent_core)
  else()
    target_link_libraries(threere-agent-tests PRIVATE p3d_thirdparty_headers)
  endif()
  target_compile_definitions(threere-agent-tests PRIVATE AGENT_CHILD_EXE="$<TARGET_FILE:threere-agent-child>"
    DECIMATE_EXE="${CMAKE_BINARY_DIR}/apps/pixel3d-decimate-guarded.exe"
    AGENT_EXE="${CMAKE_BINARY_DIR}/apps/3re-agent.exe" GODOT_EXE="${THREERE_TEST_GODOT}" WORKERS_DIR="${CMAKE_BINARY_DIR}/apps" TOOLS_DIR="${CMAKE_BINARY_DIR}/tools")
  pixel3d_executable(threere-agent-tests)
  foreach(dep threere-agent pixel3d-decimate-guarded pixel3d-asset-inspect pixel3d-geometry-audit pixel3d-glbcheck)
    if(TARGET ${dep})
      add_dependencies(threere-agent-tests ${dep})
    endif()
  endforeach()
  add_test(NAME agent-core COMMAND threere-agent-tests)
  # Asset-quality steps G0-G4 (docs/agent/ASSET-QUALITY.md).
  if(TARGET p3d_agent_core)
    add_executable(threere-asset-quality-tests agent/test_asset_quality.cpp)
    target_include_directories(threere-asset-quality-tests PRIVATE ${CMAKE_SOURCE_DIR}/apps)
    target_link_libraries(threere-asset-quality-tests PRIVATE p3d_agent_core)
    pixel3d_executable(threere-asset-quality-tests)
    add_test(NAME asset-quality COMMAND threere-asset-quality-tests)
  endif()
  # Milestone 4 chat agent (apps/agent/chat).
  if(TARGET p3d_agent_chat)
    add_executable(threere-chat-tests agent/test_chat.cpp)
    target_include_directories(threere-chat-tests PRIVATE ${CMAKE_SOURCE_DIR}/apps)
    target_link_libraries(threere-chat-tests PRIVATE p3d_agent_chat)
    pixel3d_executable(threere-chat-tests)
    add_test(NAME agent-chat COMMAND threere-chat-tests)
  endif()
endif()
