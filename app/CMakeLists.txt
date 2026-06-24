if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  add_executable(wish-server
      wish_server/wish_server.cpp
      wish_server/main.cpp)
  target_include_directories(wish-server PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "app/wish_server/..." and "src/..."
      ${CMAKE_SOURCE_DIR}/include       # for <wish/...>
      ${CMAKE_SOURCE_DIR}/extern/bison  # for "src/app/server/server_app.hpp" etc.
  )
  target_link_libraries(wish-server PRIVATE wish gflags)
  set_target_properties(wish-server PROPERTIES
      OUTPUT_NAME "wish"
      FOLDER      app)
endif()
