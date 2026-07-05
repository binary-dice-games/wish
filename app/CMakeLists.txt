if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  # ── wish-cli — unified multi-mode binary (server + client + bridge + standalone) ─
  add_executable(wish-cli
      wish_cli/main.cpp
      wish_cli/server/wish_server_app.cpp
      wish_cli/client/wish_client_app.cpp
      wish_cli/client/apps/app_registry.cpp
      wish_cli/client/apps/calculator.cpp
      wish_cli/client/apps/notepad.cpp
      wish_cli/client/apps/process_explorer/process_explorer.cpp
      wish_cli/client/apps/process_explorer/process_info_linux.cpp
      wish_cli/standalone/wish_standalone_app.cpp
      wish_cli/bridge/wish_bridge_app.cpp)
  target_include_directories(wish-cli PRIVATE
      ${CMAKE_SOURCE_DIR}               # for "app/wish_cli/..." and "src/..."
      ${CMAKE_SOURCE_DIR}/src           # for <wish/...>
      ${CMAKE_SOURCE_DIR}/extern/bison  # for "src/app/server/server_app.hpp" etc.
  )
  target_link_libraries(wish-cli PRIVATE wish gflags)
  set_target_properties(wish-cli PROPERTIES
      OUTPUT_NAME "wish"
      FOLDER      app)

  # ── wish-server — standalone server binary (requires SDL3 + ImGui) ────────────
  add_executable(wish-server
      wish_cli/server/wish_server_app.cpp
      wish_cli/server/standalone_main.cpp)
  target_include_directories(wish-server PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/extern/bison
  )
  target_link_libraries(wish-server PRIVATE wish_server gflags)
  set_target_properties(wish-server PROPERTIES FOLDER app)

  # ── wish-standalone — standalone in-process binary (requires SDL3 + ImGui) ────
  add_executable(wish-standalone
      wish_cli/standalone/wish_standalone_app.cpp
      wish_cli/standalone/standalone_main.cpp
      wish_cli/client/apps/app_registry.cpp
      wish_cli/client/apps/calculator.cpp
      wish_cli/client/apps/notepad.cpp
      wish_cli/client/apps/process_explorer/process_explorer.cpp
      wish_cli/client/apps/process_explorer/process_info_linux.cpp)
  target_include_directories(wish-standalone PRIVATE
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_SOURCE_DIR}/extern/bison
  )
  target_link_libraries(wish-standalone PRIVATE wish_server gflags)
  set_target_properties(wish-standalone PROPERTIES FOLDER app)
endif()

# ── wish-client — standalone client binary (lightweight, no SDL/ImGui) ─────────
add_executable(wish-client
    wish_cli/client/wish_client_app.cpp
    wish_cli/client/apps/app_registry.cpp
    wish_cli/client/apps/calculator.cpp
    wish_cli/client/apps/notepad.cpp
    wish_cli/client/apps/process_explorer/process_explorer.cpp
    wish_cli/client/apps/process_explorer/process_info_linux.cpp
    wish_cli/client/standalone_main.cpp)
target_include_directories(wish-client PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/bison
)
target_link_libraries(wish-client PRIVATE wish_client gflags)
set_target_properties(wish-client PROPERTIES FOLDER app)

# ── wish-bridge — standalone bridge binary (bison only, no SDL/ImGui) ──────────
add_executable(wish-bridge
    wish_cli/bridge/wish_bridge_app.cpp
    wish_cli/bridge/standalone_main.cpp)
target_include_directories(wish-bridge PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/bison
)
target_link_libraries(wish-bridge PRIVATE bison gflags)
set_target_properties(wish-bridge PROPERTIES FOLDER app)
