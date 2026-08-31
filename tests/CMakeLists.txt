# GoogleTest lives inside bison's extern; add it directly since bison's own
# test suite is disabled in the wish build context.
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/extern/bison/extern/googletest"
    "${CMAKE_BINARY_DIR}/googletest"
)

set_target_properties(gtest      PROPERTIES FOLDER extern)
set_target_properties(gtest_main PROPERTIES FOLDER extern)
set_target_properties(gmock      PROPERTIES FOLDER extern)
set_target_properties(gmock_main PROPERTIES FOLDER extern)

# Helper macro: creates a test executable linked against wish and GoogleTest.
macro(wish_add_test TESTNAME)
    add_executable(${TESTNAME} ${ARGN})
    target_link_libraries(${TESTNAME} PRIVATE wish gtest gmock gtest_main)
    gtest_discover_tests(${TESTNAME})
    set_target_properties(${TESTNAME} PROPERTIES FOLDER tests)
endmacro()

wish_add_test(test_stub              test_stub.cpp)
wish_add_test(test_resource_store    test_resource_store.cpp)
# Needs miniz.h directly to independently recompute CRC-32 for its own
# assertions (see ResourceStore.ExtractToPopulatesCrc32MapMatchingFileContent) --
# wish_server links miniz PRIVATE, which puts it on this target's link line
# already, but not its include path.
target_link_libraries(test_resource_store PRIVATE miniz)
wish_add_test(test_form_base         test_form_base.cpp)
wish_add_test(test_file_dialog  test_file_dialog.cpp)
wish_add_test(test_message_box  test_message_box.cpp)
wish_add_test(test_properties_dialog test_properties_dialog.cpp)
wish_add_test(test_registry       test_registry.cpp)
wish_add_test(test_object_inspector test_object_inspector.cpp)
wish_add_test(test_ui_importer    test_ui_importer.cpp)
wish_add_test(test_examples_ui    test_examples_ui.cpp)
target_compile_definitions(test_examples_ui PRIVATE
    WISH_EXAMPLES_UI_DIR="${CMAKE_SOURCE_DIR}/examples/ui")
wish_add_test(test_ui_element     test_ui_element.cpp)
wish_add_test(test_ui_schema_help test_ui_schema_help.cpp)
wish_add_test(test_children_order test_children_order.cpp)
wish_add_test(test_context        test_context.cpp)
wish_add_test(test_renderer       test_renderer.cpp)
wish_add_test(test_file_service   test_file_service.cpp)
# Needs miniz_zip.h directly to build zip fixtures for unpack() tests --
# wish_server links miniz PRIVATE, which puts it on this target's link line
# already, but not its include path (same pattern as test_resource_store above).
target_link_libraries(test_file_service PRIVATE miniz)
# Needs civetweb.h directly to run a throwaway HTTP server (mg_start) as a
# fixture for resolve_or_fetch()'s URL-download tests -- wish_server links
# civetweb-c-library PRIVATE, which puts it on this target's link line
# already, but not its include path (same pattern as miniz above).
target_link_libraries(test_file_service PRIVATE civetweb-c-library)
wish_add_test(test_logger         test_logger.cpp)
wish_add_test(test_server         test_server.cpp)
wish_add_test(test_standalone     test_standalone.cpp)
wish_add_test(test_client         test_client.cpp)
# Needs miniz_zip.h directly to build a zip fixture for the upload_package
# test (same pattern as test_file_service/test_resource_store above).
target_link_libraries(test_client PRIVATE miniz)
wish_add_test(test_handlers       test_handlers.cpp)
wish_add_test(test_integration    test_integration.cpp)
wish_add_test(test_auth           test_auth.cpp)

# Exercises wish_desktop's chrome-building behavior (menu bar + dockspace +
# clock) against a real wish::server upstream. wish_desktop_app.cpp isn't
# part of the `wish` library (it's an app/ source, compiled directly into
# wish-cli/wish-desktop), so it's added as an extra source here, along with
# gflags for the DEFINE_string calls it makes at file scope.
add_executable(test_wish_desktop
    test_wish_desktop.cpp
    ${CMAKE_SOURCE_DIR}/app/wish_cli/desktop/wish_desktop_app.cpp
    ${CMAKE_SOURCE_DIR}/app/wish_cli/env_flags.cpp)
target_include_directories(test_wish_desktop PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_wish_desktop PRIVATE wish gflags gtest gmock gtest_main)
gtest_discover_tests(test_wish_desktop)
set_target_properties(test_wish_desktop PROPERTIES FOLDER tests)

# Links ONLY wish_client (not the combined `wish` target) to prove the
# lightweight client / heavy server CMake split actually works for a real
# thin client -- see tests/test_client_only_link.cpp.
add_executable(test_client_only_link test_client_only_link.cpp)
target_link_libraries(test_client_only_link PRIVATE wish_client gtest gmock gtest_main)
gtest_discover_tests(test_client_only_link)
set_target_properties(test_client_only_link PROPERTIES FOLDER tests)

# Exercises apply_env_flag_defaults() (app/wish_cli/env_flags.hpp/.cpp), an
# app/ source not part of the `wish` library, so it's added as an extra
# source here (mirrors test_wish_desktop above).
add_executable(test_env_flags
    test_env_flags.cpp
    ${CMAKE_SOURCE_DIR}/app/wish_cli/env_flags.cpp)
target_include_directories(test_env_flags PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_env_flags PRIVATE gflags gtest gmock gtest_main)
gtest_discover_tests(test_env_flags)
set_target_properties(test_env_flags PROPERTIES FOLDER tests)

if(WISH_ENABLE_IMGUI)
  wish_add_test(test_imgui_renderer test_imgui_renderer.cpp)
  wish_add_test(test_imgui_layout   test_imgui_layout.cpp)
  wish_add_test(test_drag_drop      test_drag_drop.cpp)
  wish_add_test(test_style_service  test_style_service.cpp)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  wish_add_test(test_sdl3_renderer test_sdl3_renderer.cpp)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3 AND WISH_ENABLE_AUTOMATION)
  wish_add_test(test_sdl3_automation test_sdl3_automation.cpp)
  # Parses query_tree()'s TREE_SNAPSHOT-shaped JSON reply, same as
  # test_automation_query.cpp.
  target_include_directories(test_sdl3_automation PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/bison/extern/json/single_include)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_WEB)
  wish_add_test(test_web_renderer test_web_renderer.cpp)
  # Needs miniz.h directly to independently recompute CRC-32 for its own
  # assertions (see GetOrLoadTexture_ComputesCrc32OnTheFlyWhenNoPrecomputedMap).
  target_link_libraries(test_web_renderer PRIVATE miniz)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_WEB AND WISH_ENABLE_AUTOMATION)
  wish_add_test(test_automation_query test_automation_query.cpp)
  # automation_query.hpp/.cpp are the only wish_server sources that need
  # nlohmann/json.hpp directly; this test parses TREE_SNAPSHOT payloads with
  # it too (see src/automation/automation_query.cpp for the same path).
  target_include_directories(test_automation_query PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/bison/extern/json/single_include)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_AUTOMATION)
  # Renderer-agnostic: exercises automation_service's RMI plumbing against a
  # fake automation_backend, so it needs no SDL3/web renderer at all -- see
  # test_automation_service.cpp's own header comment.
  wish_add_test(test_automation_service test_automation_service.cpp)
  target_include_directories(test_automation_service PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/bison/extern/json/single_include)
endif()


# Like wish_add_test, but only registers the test when OPTION_NAME is set
# (e.g. a WISH_MODULE_<NAME> option) -- for tests that exercise an optional
# module and would otherwise fail to link when it's disabled.
macro(wish_add_optional_test OPTION_NAME TESTNAME)
    if(${OPTION_NAME})
        wish_add_test(${TESTNAME} ${ARGN})
    endif()
endmacro()

wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_TAIL               test_tail              test_tail.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_NANO               test_nano              test_nano.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_TOP                test_top               test_top.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DEV_EDITOR                test_editor            test_editor.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_MC                 test_mc                test_mc.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_ZIP                test_zip               test_zip.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_PIX                test_pix               test_pix.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_GIT                test_git               test_git.cpp)
# Pure algorithm, no bison/RMI dependency -- still gated on the module
# option since it lives under modules/bdg/desktop/git/ and would otherwise
# fail to link when that directory isn't part of the build.
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_GIT                test_git_graph_layout  test_git_graph_layout.cpp
    ${CMAKE_SOURCE_DIR}/modules/bdg/desktop/git/server/git_graph_layout.cpp)
if(TARGET test_git_graph_layout)
  target_include_directories(test_git_graph_layout PRIVATE ${CMAKE_SOURCE_DIR})
endif()

# Exercises resolve_repo_root() (git_process.cpp) against a real, throwaway
# `git init`'d repo -- client-side, no bison/RMI dependency, same pattern as
# test_git_graph_layout above. Needs uv_a (libuv) directly since git_process.cpp
# calls uv_spawn(); wish links it PRIVATE, so it doesn't propagate to this
# target automatically -- same rationale as wish_finalize_app_modules()'s own
# uv_a linking for module client targets (cmake/WishModules.cmake).
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_GIT                test_git_process       test_git_process.cpp
    ${CMAKE_SOURCE_DIR}/modules/bdg/desktop/git/client/git_process.cpp)
if(TARGET test_git_process)
  target_include_directories(test_git_process PRIVATE ${CMAKE_SOURCE_DIR})
  if(TARGET uv_a)
    target_link_libraries(test_git_process PRIVATE uv_a)
  endif()
endif()

# Exercises process_control_linux.cpp directly (kill/pause/resume/priority/
# affinity/details against a real forked child) -- pure POSIX, no bison/RMI
# dependency, same pattern as test_git_graph_layout above. Linux/MSYS2 only
# (matches that file's own scope -- see its header comment); guarded here
# with UNIX (not WIN32/MSYS2 detection) since a native-Windows build has no
# way to compile it, unlike the module's own client sources which pick
# process_control_win.cpp instead via wish_filter_platform_sources().
if(UNIX)
  wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_TOP  test_process_control_linux  test_process_control_linux.cpp
      ${CMAKE_SOURCE_DIR}/modules/bdg/desktop/top/client/process_control_linux.cpp)
  if(TARGET test_process_control_linux)
    target_include_directories(test_process_control_linux PRIVATE ${CMAKE_SOURCE_DIR})
  endif()
endif()
