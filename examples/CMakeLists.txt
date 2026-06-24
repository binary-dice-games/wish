# Example targets are added per implementation step.

if(WISH_ENABLE_SDL3)
  add_executable(calculator calculator/main.cpp)
  target_link_libraries(calculator PRIVATE wish gflags)
  target_include_directories(calculator PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "src/rmi/rmi.hpp"
      ${CMAKE_SOURCE_DIR}/extern/bison  # bison public headers
  )
  set_target_properties(calculator PROPERTIES FOLDER examples)

  add_executable(demo demo/main.cpp)
  target_link_libraries(demo PRIVATE wish gflags)
  target_include_directories(demo PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "src/rmi/rmi.hpp"
      ${CMAKE_SOURCE_DIR}/extern/bison  # bison public headers
  )
  set_target_properties(demo PROPERTIES FOLDER examples)
endif()

# config_panel demonstrates the C ABI; requires the wish_client shared library.
if(WISH_BUILD_SHARED)
  add_executable(config_panel config_panel/config_panel.cpp)
  target_link_libraries(config_panel PRIVATE wish_client gflags)
  target_include_directories(config_panel PRIVATE ${CMAKE_SOURCE_DIR}/include)
  set_target_properties(config_panel PROPERTIES FOLDER examples)

  # Copy wish_client.dll next to config_panel.exe so it can be run in-place.
  add_custom_command(TARGET config_panel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:wish_client>
      $<TARGET_FILE_DIR:config_panel>
  )
endif()
