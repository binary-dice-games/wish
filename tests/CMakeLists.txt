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
    if(MSVC)
        target_compile_options(${TESTNAME} PRIVATE /FS)
    endif()
    gtest_discover_tests(${TESTNAME})
    set_target_properties(${TESTNAME} PROPERTIES FOLDER tests)
endmacro()

wish_add_test(test_stub           test_stub.cpp)
wish_add_test(test_registry       test_registry.cpp)
wish_add_test(test_ui_importer    test_ui_importer.cpp)
wish_add_test(test_children_order test_children_order.cpp)
wish_add_test(test_session        test_session.cpp)
wish_add_test(test_renderer       test_renderer.cpp)
wish_add_test(test_file_service   test_file_service.cpp)
wish_add_test(test_server         test_server.cpp)
