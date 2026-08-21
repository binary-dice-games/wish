# MIT License © 2025 Binary Dice Games
#
# Header-only C++ binding for the wish client -- built entirely on the C ABI
# (wish_client_c.h / bison_c.h / rmi_c.h) plus the prebuilt wish_client_dll
# shared library, never on wish/bison's compiled C++ sources. Only present
# when WISH_BUILD_SHARED provides wish_client_dll to link against.

add_library(wish_cpp INTERFACE)
target_include_directories(wish_cpp INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include              # wish_client_c.h
    ${CMAKE_SOURCE_DIR}/extern/bison/include # bison_c.h / rmi_c.h
)
target_link_libraries(wish_cpp INTERFACE wish_client_dll)
target_compile_features(wish_cpp INTERFACE cxx_std_17)

# wish_cpp_server (server.hpp) is a separate INTERFACE target from wish_cpp:
# wish_server_dll is a distinct shared library from wish_client_dll (only
# present when WISH_BUILD_SERVER_SHARED=ON) -- see server.hpp's file doc
# comment for why a binary must not link both wish_client_dll and
# wish_server_dll while also using wish_cpp::value.
if(TARGET wish_server_dll)
  add_library(wish_cpp_server INTERFACE)
  target_include_directories(wish_cpp_server INTERFACE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/include              # wish_server_c.h
      ${CMAKE_SOURCE_DIR}/extern/bison/include # bison_c.h / rmi_c.h
  )
  target_link_libraries(wish_cpp_server INTERFACE wish_server_dll)
  target_compile_features(wish_cpp_server INTERFACE cxx_std_17)
endif()

add_executable(calculator_cpp_binding examples/calculator.cpp)
target_link_libraries(calculator_cpp_binding PRIVATE wish_cpp)
set_target_properties(calculator_cpp_binding PROPERTIES FOLDER "bindings/cpp/examples")

add_executable(nano_cpp_binding examples/nano.cpp)
target_link_libraries(nano_cpp_binding PRIVATE wish_cpp)
set_target_properties(nano_cpp_binding PROPERTIES FOLDER "bindings/cpp/examples")

if(WIN32 OR MSYS OR CYGWIN)
  foreach(cpp_binding_exe calculator_cpp_binding nano_cpp_binding)
    add_custom_command(TARGET ${cpp_binding_exe} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:wish_client_dll>
                $<TARGET_FILE_DIR:${cpp_binding_exe}>
    )
  endforeach()
endif()

if(WISH_BUILD_TESTS)
  # The binding itself only ever touches the C ABI (wish_cpp above), but the
  # test fixture also spins up a real wish::server (socket transport) as the
  # binding client's counterpart, so this target additionally links the full
  # `wish` library (client + server) purely for that server-side half.
  add_executable(wish_cpp_binding_tests tests/test_client.cpp)
  target_link_libraries(wish_cpp_binding_tests PRIVATE wish_cpp wish gtest gtest_main)
  target_include_directories(wish_cpp_binding_tests PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "src/rmi/transport/socket_transport.hpp"
      ${CMAKE_SOURCE_DIR}/extern/bison  # bison public headers
  )
  set_target_properties(wish_cpp_binding_tests PROPERTIES FOLDER "bindings/cpp/tests")
  if(WIN32 OR MSYS OR CYGWIN)
    add_custom_command(TARGET wish_cpp_binding_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:wish_client_dll>
                $<TARGET_FILE_DIR:wish_cpp_binding_tests>
    )
  endif()
  gtest_discover_tests(wish_cpp_binding_tests)

  if(TARGET wish_cpp_server)
    add_executable(wish_cpp_server_binding_tests tests/test_server.cpp)
    target_link_libraries(wish_cpp_server_binding_tests PRIVATE wish_cpp_server gtest gtest_main)
    set_target_properties(wish_cpp_server_binding_tests PROPERTIES FOLDER "bindings/cpp/tests")
    if(WIN32 OR MSYS OR CYGWIN)
      add_custom_command(TARGET wish_cpp_server_binding_tests POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  $<TARGET_FILE:wish_server_dll>
                  $<TARGET_FILE_DIR:wish_cpp_server_binding_tests>
      )
    endif()
    gtest_discover_tests(wish_cpp_server_binding_tests)
  endif()
endif()
