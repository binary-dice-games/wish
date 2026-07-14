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
wish_add_test(test_registry       test_registry.cpp)
wish_add_test(test_ui_importer    test_ui_importer.cpp)
wish_add_test(test_children_order test_children_order.cpp)
wish_add_test(test_context        test_context.cpp)
wish_add_test(test_renderer       test_renderer.cpp)
wish_add_test(test_file_service   test_file_service.cpp)
# Needs miniz_zip.h directly to build zip fixtures for unpack() tests --
# wish_server links miniz PRIVATE, which puts it on this target's link line
# already, but not its include path (same pattern as test_resource_store above).
target_link_libraries(test_file_service PRIVATE miniz)
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
  wish_add_test(test_style_service  test_style_service.cpp)
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


# Like wish_add_test, but only registers the test when OPTION_NAME is set
# (e.g. a WISH_MODULE_<NAME> option) -- for tests that exercise an optional
# module and would otherwise fail to link when it's disabled.
macro(wish_add_optional_test OPTION_NAME TESTNAME)
    if(${OPTION_NAME})
        wish_add_test(${TESTNAME} ${ARGN})
    endif()
endmacro()

wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_NOTEPAD           test_notepad           test_notepad.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DESKTOP_PROCESS_EXPLORER  test_process_explorer  test_process_explorer.cpp)
wish_add_optional_test(WISH_MODULE_BDG_DEV_EDITOR                test_editor            test_editor.cpp)
