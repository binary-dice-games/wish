if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  add_executable(wish-cli
      wish_cli/main.cpp
      wish_cli/server/wish_server_app.cpp
      wish_cli/client/wish_client_app.cpp
      wish_cli/client/apps/calculator.cpp
      wish_cli/bridge/wish_bridge_app.cpp)
  target_include_directories(wish-cli PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "app/wish_cli/..." and "src/..."
      ${CMAKE_SOURCE_DIR}/include       # for <wish/...>
      ${CMAKE_SOURCE_DIR}/extern/bison  # for "src/app/server/server_app.hpp" etc.
  )
  target_link_libraries(wish-cli PRIVATE wish gflags)
  set_target_properties(wish-cli PROPERTIES
      OUTPUT_NAME "wish"
      FOLDER      app)
endif()
