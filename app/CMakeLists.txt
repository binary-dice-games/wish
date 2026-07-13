# wish's own root -- not CMAKE_SOURCE_DIR, which points at the outer
# project's root instead when wish is consumed via add_subdirectory().
set(WISH_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Module client sources/defs (WISH_APP_MODULE_SOURCES / _DEFS) are applied
# to the targets below by wish_finalize_app_modules() (see
# cmake/WishModules.cmake), not here -- that call must happen after every
# wish_add_module() call, including any a consuming project issues after
# add_subdirectory(wish) returns, which is later than this file runs.

if(WISH_ENABLE_IMGUI AND (WISH_ENABLE_SDL3 OR WISH_ENABLE_WEB))
  # ── wish-cli — unified multi-mode binary (server + client + desktop + standalone) ─
  #
  # Builds with either (or both) renderer backends; wish_server_app.cpp picks
  # between them at runtime via --renderer, guarded by WISH_SDL3_ENABLED /
  # WISH_WEB_ENABLED so a backend that wasn't compiled in simply isn't an
  # option (see wish_server_app.cpp's make_renderer()).
  add_executable(wish-cli
      wish_cli/main.cpp
      wish_cli/env_flags.cpp
      wish_cli/server/wish_server_app.cpp
      wish_cli/client/wish_client_app.cpp
      wish_cli/standalone/wish_standalone_app.cpp
      wish_cli/desktop/wish_desktop_app.cpp)
  target_include_directories(wish-cli PRIVATE
      ${WISH_ROOT_DIR}               # for "app/wish_cli/..." and "modules/.../..." and "src/..."
      ${WISH_ROOT_DIR}/src           # for <wish/...>
      ${WISH_ROOT_DIR}/extern/bison  # for "src/app/server/server_app.hpp" etc.
  )
  target_link_libraries(wish-cli PRIVATE wish gflags)
  target_compile_definitions(wish-cli PRIVATE WISH_CLI_BUILD)
  set_target_properties(wish-cli PROPERTIES
      OUTPUT_NAME "wish"
      FOLDER      app)

  # ── wish-server — standalone server binary (requires ImGui + SDL3 and/or web) ─
  add_executable(wish-server
      wish_cli/env_flags.cpp
      wish_cli/server/wish_server_app.cpp
      wish_cli/server/standalone_main.cpp)
  target_include_directories(wish-server PRIVATE
      ${WISH_ROOT_DIR}
      ${WISH_ROOT_DIR}/src
      ${WISH_ROOT_DIR}/extern/bison
  )
  target_link_libraries(wish-server PRIVATE wish_server gflags)
  set_target_properties(wish-server PROPERTIES FOLDER app)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  # ── wish-standalone — standalone in-process binary (requires SDL3 + ImGui) ────
  #
  # Runs server and client in one process with no transport; this is
  # inherently an interactive, windowed use case, so unlike wish-cli/
  # wish-server it does not build under WISH_ENABLE_WEB alone.
  add_executable(wish-standalone
      wish_cli/standalone/wish_standalone_app.cpp
      wish_cli/standalone/standalone_main.cpp)
  target_include_directories(wish-standalone PRIVATE
      ${WISH_ROOT_DIR}
      ${WISH_ROOT_DIR}/src
      ${WISH_ROOT_DIR}/extern/bison
  )
  target_link_libraries(wish-standalone PRIVATE wish_server gflags)
  set_target_properties(wish-standalone PROPERTIES FOLDER app)
endif()

# ── wish-client — standalone client binary (lightweight, no SDL/ImGui) ─────────
add_executable(wish-client
    wish_cli/env_flags.cpp
    wish_cli/client/wish_client_app.cpp
    wish_cli/client/standalone_main.cpp)
target_include_directories(wish-client PRIVATE
    ${WISH_ROOT_DIR}
    ${WISH_ROOT_DIR}/src
    ${WISH_ROOT_DIR}/extern/bison
)
target_link_libraries(wish-client PRIVATE wish_client gflags)
set_target_properties(wish-client PROPERTIES FOLDER app)

# ── wish-desktop — standalone bridge+shell binary (no SDL/ImGui) ───────────────
#
# Links wish_client (not just bison) for import_descriptor_json(), used to
# build the desktop shell's UI template descriptor client-side; wish_client
# has no ImGui/SDL dependency, keeping this binary's footprint lightweight.
add_executable(wish-desktop
    wish_cli/env_flags.cpp
    wish_cli/desktop/wish_desktop_app.cpp
    wish_cli/desktop/standalone_main.cpp)
target_include_directories(wish-desktop PRIVATE
    ${WISH_ROOT_DIR}
    ${WISH_ROOT_DIR}/src
    ${WISH_ROOT_DIR}/extern/bison
)
target_link_libraries(wish-desktop PRIVATE wish_client gflags)
set_target_properties(wish-desktop PROPERTIES FOLDER app)
