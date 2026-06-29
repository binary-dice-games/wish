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
