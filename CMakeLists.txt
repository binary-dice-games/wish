# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Wish
  VERSION 1.0
  LANGUAGES CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(WISH_BUILD_TESTS      "Build the wish tests"                            ON)
option(WISH_ENABLE_IMGUI     "Build the imgui renderer"                       ON)
option(WISH_ENABLE_SDL3      "Build the SDL3 renderer and calculator example" ON)
option(WISH_ENABLE_WEB       "Build the web renderer (browser + WebSocket backend)" ON)
option(WISH_ENABLE_AUTOMATION "Build the automation query API (headless UI automation for \
     AI agents / e2e tests): browser-driven via Playwright when WISH_ENABLE_WEB is on, and/or \
     native ABI-driven automation (no browser) when WISH_ENABLE_SDL3 is on -- requires at \
     least one of the two" OFF)
option(WISH_BUILD_SHARED     "Build wish_client shared library with C ABI"   ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Automation has no renderer to extend without at least one of the two
# backends that implement it: the web renderer's browser/WebSocket surface
# (src/web/), or the SDL3 renderer's native ABI-driven surface
# (src/automation/DESIGN.md's "Native (ABI-based) automation" section).
if(WISH_ENABLE_AUTOMATION AND NOT (WISH_ENABLE_WEB OR WISH_ENABLE_SDL3))
  message(FATAL_ERROR "WISH_ENABLE_AUTOMATION requires WISH_ENABLE_WEB=ON and/or WISH_ENABLE_SDL3=ON")
endif()

# Disable bison's own test suite in this build; wish manages its own tests
# and adds GoogleTest directly from bison's extern/ directory.
set(PACKAGE_TESTS OFF CACHE BOOL "Build bison tests" FORCE)
add_subdirectory("extern/bison")

# extern/* dependencies are git submodules (see .gitmodules), not fetched via
# FetchContent -- a plain `git submodule update --init --recursive` has no
# concept of the WISH_ENABLE_* options below and would unconditionally check
# out every optional dependency's source tree. wish_ensure_submodules()
# reproduces FetchContent's old lazy-fetch behavior: only clone the paths a
# given if(WISH_ENABLE_...) block actually needs. A no-op (and network-free)
# once a submodule is already checked out.
function(wish_ensure_submodules)
  # CMAKE_CURRENT_SOURCE_DIR, not CMAKE_SOURCE_DIR: this function runs in
  # wish's own directory scope even when wish is add_subdirectory()'d from
  # an outer project (function() calls don't change directory scope), but
  # CMAKE_SOURCE_DIR always names the outermost project's root -- using it
  # here would run `git submodule update` against the wrong repository.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND git submodule update --init -- ${ARGN}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      RESULT_VARIABLE _wish_submodule_result
    )
    if(NOT _wish_submodule_result EQUAL 0)
      message(FATAL_ERROR "Failed to init git submodule(s): ${ARGN}")
    endif()
  endif()
endfunction()

if(WISH_ENABLE_IMGUI)
  wish_ensure_submodules(extern/imgui extern/implot extern/implot3d extern/imgui_color_text_edit)
  set(imgui_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui)
  set(implot_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/implot)
  set(implot3d_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/implot3d)
  set(imgui_color_text_edit_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui_color_text_edit)

  add_library(imgui_core STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot3d_SOURCE_DIR}/implot3d.cpp
    ${implot3d_SOURCE_DIR}/implot3d_items.cpp
    ${implot3d_SOURCE_DIR}/implot3d_meshes.cpp
    ${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp
    ${imgui_color_text_edit_SOURCE_DIR}/TextDiff.cpp
  )
  target_include_directories(imgui_core PUBLIC
    ${imgui_SOURCE_DIR}
    ${implot_SOURCE_DIR}
    ${implot3d_SOURCE_DIR}
    ${imgui_color_text_edit_SOURCE_DIR}
  )
  set_target_properties(imgui_core PROPERTIES FOLDER extern)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  wish_ensure_submodules(extern/sdl3 extern/sdl3_image)
  set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
  # Disable AVIF (needs dav1d + NASM) and JXL before SDL3_image is configured.
  set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
  set(SDLIMAGE_JXL  OFF CACHE BOOL "" FORCE)
  add_subdirectory(extern/sdl3)
  add_subdirectory(extern/sdl3_image)

  # imgui docking branch ships SDL3 backend sources; add them to imgui_core.
  target_sources(imgui_core PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_core PUBLIC SDL3::SDL3)
endif()

# ── civetweb — HTTP/WebSocket server ─────────────────────────────────────────
#
# Unconditional (not gated by WISH_ENABLE_IMGUI/WISH_ENABLE_WEB): net::http_get
# (src/net/http_client.hpp) previously used civetweb's client API to fetch
# http(s):// Image::src/font_path URLs, but civetweb's SSL support is Windows-
# hostile (compile-time-hardcoded dynamic-loading DLL names it doesn't expose
# a way to override, and a WebSocket-handshake crash under one of its OpenSSL
# API variants if the DLL isn't found -- see git history for the investigation).
# net::http_get is now built on libcurl instead (see the libcurl block below),
# so civetweb here is purely an HTTP+WebSocket *server* -- used by the web
# renderer as an embedded server (WISH_ENABLE_WEB, see
# src/web/civetweb_server.cpp) and by tests as a throwaway local HTTP server
# (tests/test_file_service.cpp's TestHttpServer). Neither needs TLS: the
# server side only ever binds to 127.0.0.1 for local dev/test tooling.
wish_ensure_submodules(extern/civetweb)
set(CIVETWEB_ENABLE_WEBSOCKETS        ON  CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SSL               OFF CACHE BOOL "" FORCE)
set(CIVETWEB_BUILD_TESTING            OFF CACHE BOOL "" FORCE)
set(CIVETWEB_ENABLE_SERVER_EXECUTABLE OFF CACHE BOOL "" FORCE)
set(CIVETWEB_INSTALL_EXECUTABLE       OFF CACHE BOOL "" FORCE)
# civetweb's own CMakeLists defaults this ON and adds -fsanitize=address
# -fsanitize=undefined to its Debug compile flags without propagating them
# to consumers' link flags, so any Debug build (e.g. VSCode CMake Tools'
# default config) fails to link with undefined __ubsan_handle_*/__asan_*
# references. wish doesn't build the rest of the project with sanitizers,
# so civetweb shouldn't either.
set(CIVETWEB_ENABLE_ASAN              OFF CACHE BOOL "" FORCE)
# civetweb's own CMakeLists defaults this ON in Debug builds, adding
# -DDEBUG to its compile flags. That enables civetweb's internal
# DEBUG_TRACE_FUNC, which unconditionally fprintf()s raw '\n'-terminated
# lines straight to stdout -- noisy, and (before the bison::term::terminal
# stdio redirect) a source of "staircase" output under --transport=term's
# raw-mode terminal. civetweb is a vendored stable dependency here, not
# something wish is developing, so its internal tracing has no value to us.
set(CIVETWEB_ENABLE_DEBUG_TOOLS        OFF CACHE BOOL "" FORCE)
# civetweb v1.16's own CMakeLists declares cmake_minimum_required(3.3),
# which CMake 4+ refuses to configure without an explicit policy floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(extern/civetweb)
unset(CMAKE_POLICY_VERSION_MINIMUM)
set_target_properties(civetweb-c-library PROPERTIES FOLDER extern)
# civetweb's master accept-loop only checks its stop flag once per
# SOCKET_TIMEOUT_QUANTUM (2000ms by default), so mg_stop() can otherwise
# block for up to 2s -- multiplied across every test that starts a
# civetweb_server. This only affects shutdown latency, not runtime
# behavior.
target_compile_definitions(civetweb-c-library PRIVATE SOCKET_TIMEOUT_QUANTUM=100)

# ── libcurl — HTTP/HTTPS client ──────────────────────────────────────────────
#
# Backs net::http_get (src/net/http_client.cpp), used by
# file_service::resolve_or_fetch to download http(s):// resource URLs.
# Windows uses curl's Schannel backend (native OS TLS, no external OpenSSL
# DLL dependency); Linux/MSYS2 link the ambient system OpenSSL, same as
# every other TLS-using tool on those platforms.
wish_ensure_submodules(extern/curl)
set(BUILD_CURL_EXE       OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON  CACHE BOOL "" FORCE)
set(BUILD_TESTING        OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL      OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB            OFF CACHE BOOL "" FORCE)
# net::http_get only needs plain http(s) GET; disable every other protocol
# curl supports to shrink the build and its dependency surface.
foreach(proto IN ITEMS DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET TFTP)
  set(CURL_DISABLE_${proto} ON CACHE BOOL "" FORCE)
endforeach()
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL  ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(extern/curl)
# "libcurl" is only an ALIAS (curl's own CMakeLists.txt) for whichever of
# libcurl_static/libcurl_shared BUILD_SHARED_LIBS selected -- set_target_properties
# rejects ALIAS targets, so target the real one directly.
set_target_properties(libcurl_static PROPERTIES FOLDER extern)

if(WISH_ENABLE_IMGUI AND (WISH_ENABLE_WEB OR (WISH_ENABLE_SDL3 AND WISH_ENABLE_AUTOMATION)))
  # Single-header PNG/JPEG codec, vendored via extern/stb: stb_image.h (read
  # side) backs web_renderer::get_or_load_texture -- WISH_ENABLE_SDL3 (and
  # its transitive SDL3_image dependency) is not guaranteed to be on in a
  # web-only build, so this backend needs its own decoder. stb_image_write.h
  # (write side) backs sdl3_renderer's automation screenshot capture
  # (capture_frame_png(), see src/automation/DESIGN.md) when automation is
  # enabled alongside SDL3.
  wish_ensure_submodules(extern/stb)
  add_library(stb_image INTERFACE)
  target_include_directories(stb_image INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb)
endif()

# ── miniz — zip (de)compression, used to unpack embedded resources ───────────

wish_ensure_submodules(extern/miniz)
# miniz's own CMakeLists defaults these ON only when built standalone; force
# off defensively, matching the SDLIMAGE_AVIF/SDLIMAGE_JXL pattern above.
set(BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/miniz)
set_target_properties(miniz PROPERTIES FOLDER extern)

# ── Embedded resources — zip the asset tree, then embed as one byte array ────
#
# wish_generate_embedded_resources() (cmake/WishResources.cmake) does the
# actual work; it's called at the finalize point below (alongside
# wish_generate_module_registry()/wish_finalize_app_modules()), after every
# wish_add_module()/wish_add_collection() call, so enabled modules'
# resources/embedded/ trees are folded in too.
include(cmake/WishResources.cmake)

# ── wish_client — lightweight C++ client (no renderer, no UI elements) ────────

add_library(wish_client STATIC
  src/client/client.hpp
  src/client/client.cpp
  src/client/app_registry.hpp
  src/client/app_registry.cpp
  src/client/wish_app_host.hpp
  src/ui/ui_descriptor.hpp
  src/ui/ui_descriptor.cpp
)
set_target_properties(wish_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wish_client
    PUBLIC
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
    PRIVATE
        # nlohmann/json and libyaml headers; only needed by src/ui/ui_descriptor.cpp.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/json/single_include
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/yaml/include
)
target_link_libraries(wish_client PUBLIC bison)
# libyaml is needed by ui_descriptor.cpp; PRIVATE because it is an impl detail.
target_link_libraries(wish_client PRIVATE yaml)

# ── wish_server — server, renderer, and all UI elements (heavy stack) ─────────

add_library(wish_server STATIC
  src/wish.hpp
  src/resources/resource_store.hpp
  src/server/registry.hpp
  src/ui/ui_importer.hpp
  src/ui/ui_element.hpp
  src/context/context.hpp
  src/server/renderer.hpp
  src/context/file_service.hpp
  src/context/logger.hpp
  src/context/style_service.hpp
  src/net/http_client.hpp
  src/server/server.hpp
  src/auth/local_auth_module.hpp
  src/standalone/standalone.hpp
  src/ui/forms/form.hpp
  src/ui/forms/file_browser_utils.hpp
  src/wish.cpp
  src/resources/resource_store.cpp
  src/server/registry.cpp
  src/ui/ui_importer.cpp
  src/ui/ui_element.cpp
  src/ui/ui_schema_help.cpp
  src/context/context.cpp
  src/server/renderer.cpp
  src/context/file_service.cpp
  src/context/logger.cpp
  src/context/style_service.cpp
  src/net/http_client.cpp
  src/server/server.cpp
  src/auth/local_auth_module.cpp
  src/standalone/standalone.cpp
  src/ui/ui_template.cpp
  src/ui/ui_elements/element.cpp
  src/ui/ui_elements/layout.cpp
  src/ui/ui_elements/window.cpp
  src/ui/ui_elements/label.cpp
  src/ui/ui_elements/button.cpp
  src/ui/ui_elements/checkbox.cpp
  src/ui/ui_elements/slider.cpp
  src/ui/ui_elements/input_text.cpp
  src/ui/ui_elements/image.cpp
  src/ui/ui_elements/separator.cpp
  src/ui/ui_elements/menu.cpp
  src/ui/ui_elements/tabs.cpp
  src/ui/ui_elements/tree.cpp
  src/ui/ui_elements/combo.cpp
  src/ui/ui_elements/radio_button.cpp
  src/ui/ui_elements/progress_bar.cpp
  src/ui/ui_elements/input_number.cpp
  src/ui/ui_elements/drag.cpp
  src/ui/ui_elements/selectable.cpp
  src/ui/ui_elements/docking.cpp
  src/ui/ui_elements/table.cpp
  src/ui/ui_elements/text_editor.cpp
  src/ui/ui_elements/graph_node.cpp
  src/ui/plot_elements/plot.cpp
  src/ui/plot_elements/plot_series.cpp
  src/ui/plot_elements/plot_bars.cpp
  src/ui/plot_elements/plot_histogram.cpp
  src/ui/plot_elements/plot_heatmap.cpp
  src/ui/plot_elements/plot_pie.cpp
  src/ui/plot_elements/plot_annotations.cpp
  src/ui/plot3d_elements/plot3d.cpp
  src/ui/plot3d_elements/plot3d_series.cpp
  src/ui/plot3d_elements/plot3d_surface.cpp
  src/ui/plot3d_elements/plot3d_mesh.cpp
  src/ui/plot3d_elements/plot3d_annotations.cpp
  src/ui/forms/form.cpp
  src/ui/forms/file_browser_utils.cpp
  src/ui/forms/file_dialog.cpp
  src/ui/forms/message_box.cpp
)

if(WISH_ENABLE_IMGUI)
  target_sources(wish_server PRIVATE
    src/imgui/imgui_renderer.hpp
    src/imgui/imgui_renderer.cpp
    src/imgui/imgui_ui_renderer.hpp
    src/imgui/imgui_ui_renderer.cpp
    src/imgui/imgui_plot_renderer.hpp
    src/imgui/imgui_plot_renderer.cpp
    src/imgui/imgui_plot3d_renderer.hpp
    src/imgui/imgui_plot3d_renderer.cpp
    src/imgui/imgui_text_editor_renderer.cpp
    src/imgui/imgui_graph_renderer.hpp
    src/imgui/imgui_graph_renderer.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_IMGUI_ENABLED)
  # PUBLIC so that consumers who include imgui_renderer.hpp have imgui in path.
  target_link_libraries(wish_server PUBLIC imgui_core)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  target_sources(wish_server PRIVATE
    src/sdl/sdl3_renderer.hpp
    src/sdl/sdl3_renderer.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_SDL3_ENABLED)
  target_link_libraries(wish_server PUBLIC SDL3::SDL3 SDL3_image::SDL3_image)
  if(WISH_ENABLE_AUTOMATION)
    # stb_image_write.h (screenshot PNG encoding, capture_frame_png()) is
    # isolated behind sdl3_renderer.cpp; public headers never include it.
    target_link_libraries(wish_server PRIVATE stb_image)
  endif()
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_WEB)
  target_sources(wish_server PRIVATE
    src/web/web_renderer.hpp
    src/web/web_renderer.cpp
    src/web/draw_protocol.hpp
    src/web/draw_protocol.cpp
    src/web/civetweb_server.hpp
    src/web/civetweb_server.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_WEB_ENABLED)
  # PRIVATE: stb_image is isolated behind web_renderer.cpp; public headers
  # (web_renderer.hpp) never include <stb_image.h>.
  target_link_libraries(wish_server PRIVATE stb_image)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_AUTOMATION)
  # Renderer-agnostic automation infrastructure, shared by both backends that
  # implement it: automation_query.* (tree/hit-test snapshot + log-event JSON
  # builders, used by both web_renderer and sdl3_renderer) and
  # automation_backend/automation_service (the RMI service + interface that
  # let a wish::client drive automation natively over the C ABI -- see
  # src/automation/DESIGN.md's "Native (ABI-based) automation" section, used
  # only by sdl3_renderer today but not SDL3-specific itself). Each backend's
  # own integration glue stays in its own WISH_ENABLE_WEB/WISH_ENABLE_SDL3
  # block below/above.
  target_sources(wish_server PRIVATE
    src/automation/automation_backend.hpp
    src/automation/automation_query.hpp
    src/automation/automation_query.cpp
    src/automation/automation_service.hpp
    src/automation/automation_service.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_AUTOMATION_ENABLED)
  # nlohmann/json is already vendored via bison; only automation_query.cpp
  # needs it directly (ui_descriptor.cpp's own use of it is wish_client's
  # problem, with its own PRIVATE include of the same path).
  target_include_directories(wish_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/json/single_include
  )
endif()

set_target_properties(wish_server PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(wish_server
    PRIVATE
        src
        src/resources
        # repo root; needed for "modules/<name>/server/..." includes used by
        # registry.cpp and the generated module registry (see WishModules.cmake).
        ${CMAKE_CURRENT_SOURCE_DIR}
    PUBLIC
        src
        # Exposes bison's C++ headers (src/bison/bison_object.hpp, etc.)
        # using the same include convention as bison's own examples and tests.
        # PUBLIC because wish's own public headers reference bison types.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
)

# bison is PUBLIC so consumers of wish_server also get bison's types and link deps.
target_link_libraries(wish_server PUBLIC bison)
# wish_client provides src/ui/ui_descriptor.cpp (text -> generic dynamic tree),
# which ui_importer.cpp builds on; PUBLIC so wish_server consumers also get
# client::register_template et al. without linking wish_client separately.
target_link_libraries(wish_server PUBLIC wish_client)
# miniz is needed by resource_store.cpp to unpack the embedded archive.
target_link_libraries(wish_server PRIVATE miniz)
# civetweb backs civetweb_server's embedded HTTP+WebSocket server;
# libcurl backs net::http_get (src/net/http_client.cpp). Both PRIVATE since
# <civetweb.h>/<curl/curl.h> are isolated to their respective .cpp files.
target_link_libraries(wish_server PRIVATE civetweb-c-library libcurl)

# ── Optional modules — calculator, notepad, process_explorer, ... ────────────
#
# Each module is off by default; see cmake/WishModules.cmake for the
# wish_add_module() framework and modules/<name>/{server,client}/ for the
# per-module sources. Adding a new module only requires a new
# modules/<name>/ directory and one wish_add_module() line here.
include(cmake/WishModules.cmake)

wish_add_collection(bdg/desktop DEFAULT ON)
wish_add_collection(bdg/dev)

# wish_generate_module_registry()/wish_generate_embedded_resources() are
# safe to call now (wish_server already exists) and only touch
# CMAKE_BINARY_DIR/target_sources(wish_server ...), both of which are
# well-defined whether wish is the top-level project or add_subdirectory()'d
# from a consumer -- unlike wish_finalize_app_modules() below, which needs
# wish-cli/wish-client/wish-standalone/wish_client_dll (not created yet) and
# genuinely must wait for a consumer's own wish_add_module() calls, these two
# must always run or registry.cpp/resource_store.cpp fail to link.
wish_generate_module_registry()
wish_generate_embedded_resources()

# ── wish — convenience interface target (client + server) ─────────────────────
#
# Single-binary apps (calculator, demo) and tests that need both sides link
# against this target and get everything.  External projects that only need
# one side link directly against wish_client or wish_server instead.

add_library(wish INTERFACE)
target_link_libraries(wish INTERFACE wish_server wish_client)

if(WISH_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

# ── wish_client_dll — C ABI shared library wrapping wish_client ───────────────
#
# CMake target is named wish_client_dll to avoid colliding with the wish_client
# static lib; the output filename is still wish_client.dll / libwish_client.so.
#
# Consumers only need to link/load this one library to get the full
# bison + RMI + wish C ABI surface, but bison_c.cpp and rmi_c.cpp are *not*
# listed as separate sources here: src/wish_client_c.cpp #includes both
# directly (see that file) so it can reuse their internal handle-wrapping
# helpers (bison_handle/rmi_proxy_handle/rmi_future_handle are otherwise
# opaque outside those translation units) without a second bison-side API
# surface. Compiling them again here would duplicate every bison_*/rmi_*
# symbol.

if(WISH_BUILD_SHARED)
  add_library(wish_client_dll SHARED
      src/wish_client_c.cpp
      include/wish_client_c.h
      extern/bison/include/bison_c.h
      extern/bison/include/rmi_c.h)
  target_compile_definitions(wish_client_dll PRIVATE WISH_CLIENT_BUILDING_DLL BISON_BUILDING_DLL)
  # wish_client_c.cpp #includes bison_c.cpp/rmi_c.cpp directly (see comment
  # above), so this target must mirror the same native-Windows compile
  # definitions the `bison` target itself uses (extern/bison/CMakeLists.txt)
  # for rmi_c.cpp's #if defined(BISON_NATIVE_WINDOWS) guards to take effect.
  if(WIN32 AND NOT MSYS AND NOT CYGWIN)
    target_compile_definitions(wish_client_dll PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN BISON_NATIVE_WINDOWS)
  elseif(WIN32)
    target_compile_definitions(wish_client_dll PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  target_link_libraries(wish_client_dll PUBLIC wish_client)
  target_include_directories(wish_client_dll
      PUBLIC
          ${CMAKE_CURRENT_SOURCE_DIR}/include
      PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
          # nlohmann/json, for wish_list_apps()'s JSON encoding in wish_client_c.cpp.
          ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/json/single_include)
  set_target_properties(wish_client_dll PROPERTIES
      RUNTIME_OUTPUT_NAME wish_client      # wish_client.dll
      ARCHIVE_OUTPUT_NAME wish_client_dll  # wish_client_dll.lib — avoids collision with wish_client.lib
      OUTPUT_NAME         wish_client      # MSYS2/Linux fallback
      FOLDER              lib)
  install(TARGETS wish_client_dll
      RUNTIME DESTINATION bin COMPONENT wish   # wish_client.dll / libwish_client.so (MSYS2)
      LIBRARY DESTINATION bin COMPONENT wish   # libwish_client.so (Linux)
      ARCHIVE DESTINATION lib COMPONENT wish)  # wish_client_dll.lib (MSVC import lib)
  install(FILES
      include/wish_client_c.h
      extern/bison/include/bison_c.h
      extern/bison/include/rmi_c.h
      DESTINATION include
      COMPONENT wish)
endif()

add_subdirectory(app)

# Only auto-run here when wish is the top-level project. A project that
# consumes wish via add_subdirectory()/FetchContent may still want to
# register its own out-of-tree modules (see cmake/WishModules.cmake's
# "Out-of-tree modules" section) before this is finalized -- it must call
# wish_finalize_app_modules() itself, once, after add_subdirectory(wish)
# and every wish_add_module() call (its own and wish's built-in ones).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  wish_finalize_app_modules()
endif()

add_subdirectory(examples)

# bindings/cpp — header-only C++ client binding + its examples/tests. Needs
# wish_client_dll (built above) and, for its test executable, the gtest
# targets tests/CMakeLists.txt adds -- both must already exist.
if(WISH_BUILD_SHARED)
  add_subdirectory(bindings/cpp)
endif()

# Packaging (install() destinations for non-target content + CPack ZIP
# config) only makes sense when wish is the top-level project -- a consumer
# using add_subdirectory()/FetchContent has its own packaging concerns.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(cmake/Packaging.cmake)
endif()
