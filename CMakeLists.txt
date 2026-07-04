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
option(WISH_BUILD_SHARED     "Build wish_client shared library with C ABI"   ON)
option(WISH_MODULE_DESKTOP   "Include desktop utility forms (Calculator, Notepad, ...)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Disable bison's own test suite in this build; wish manages its own tests
# and adds GoogleTest directly from bison's extern/ directory.
set(PACKAGE_TESTS OFF CACHE BOOL "Build bison tests" FORCE)
add_subdirectory("extern/bison")

if(WISH_ENABLE_IMGUI)
  include(FetchContent)
  FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
  )
  FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_Declare(
    implot3d
    GIT_REPOSITORY https://github.com/brenocq/implot3d.git
    GIT_TAG        v0.4
    GIT_SHALLOW    TRUE
  )
  FetchContent_Declare(
    imgui_color_text_edit
    GIT_REPOSITORY https://github.com/goossens/ImGuiColorTextEdit.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(imgui implot implot3d imgui_color_text_edit)

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
  include(FetchContent)
  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.10
    GIT_SHALLOW    TRUE
  )
  FetchContent_Declare(
    SDL3_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG        release-3.2.4
    GIT_SHALLOW    TRUE
  )
  # Disable AVIF (needs dav1d + NASM) and JXL before SDL3_image is configured.
  set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
  set(SDLIMAGE_JXL  OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3 SDL3_image)

  # imgui docking branch ships SDL3 backend sources; add them to imgui_core.
  target_sources(imgui_core PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_core PUBLIC SDL3::SDL3)
endif()

# ── Embedded resources — generate embedded_resources.cpp from asset tree ──────

set(WISH_RESOURCE_DIR "${CMAKE_SOURCE_DIR}/resources/embedded"
    CACHE PATH "Source asset directory compiled into the binary")

set(WISH_EMBEDDED_CPP_IN "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/embedded_resources.cpp.in")
set(WISH_EMBEDDED_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/embedded_resources.cpp")

file(GLOB_RECURSE ALL_ASSETS RELATIVE "${WISH_RESOURCE_DIR}" "${WISH_RESOURCE_DIR}/*")

set(FULL_ASSET_PATHS "")
foreach(asset ${ALL_ASSETS})
    list(APPEND FULL_ASSET_PATHS "${WISH_RESOURCE_DIR}/${asset}")
endforeach()

string(REPLACE ";" "\\;" ESCAPED_ASSETS "${ALL_ASSETS}")

add_custom_command(
    OUTPUT "${WISH_EMBEDDED_CPP}"
    COMMAND ${CMAKE_COMMAND} 
        -DWISH_RESOURCE_DIR=${WISH_RESOURCE_DIR}
        -DALL_ASSETS="${ESCAPED_ASSETS}"
        -DTEMPLATE_FILE=${WISH_EMBEDDED_CPP_IN} 
        -DOUTPUT_FILE=${WISH_EMBEDDED_CPP} 
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateResource.cmake"
    DEPENDS ${FULL_ASSET_PATHS} "${WISH_EMBEDDED_CPP_IN}"
    COMMENT "Generating embedded C++ resources table from ${WISH_RESOURCE_DIR}"
)

# ── wish_client — lightweight C++ client (no renderer, no UI elements) ────────

add_library(wish_client STATIC
  src/client.hpp
  src/client.cpp
)
set_target_properties(wish_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wish_client
    PUBLIC
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
)
target_link_libraries(wish_client PUBLIC bison)

# ── wish_server — server, renderer, and all UI elements (heavy stack) ─────────

add_library(wish_server STATIC
  src/wish.hpp
  src/resources/resource_store.hpp
  src/registry.hpp
  src/ui_importer.hpp
  src/ui_element.hpp
  src/session.hpp
  src/renderer.hpp
  src/file_service.hpp
  src/logger.hpp
  src/style_service.hpp
  src/server.hpp
  src/forms/form.hpp
  src/wish.cpp
  src/resources/resource_store.cpp
  "${WISH_EMBEDDED_CPP}"
  src/registry.cpp
  src/ui_importer.cpp
  src/ui_element.cpp
  src/session.cpp
  src/renderer.cpp
  src/file_service.cpp
  src/logger.cpp
  src/style_service.cpp
  src/server.cpp
  src/template_handler.cpp
  src/ui_elements/element.cpp
  src/ui_elements/layout.cpp
  src/ui_elements/window.cpp
  src/ui_elements/label.cpp
  src/ui_elements/button.cpp
  src/ui_elements/checkbox.cpp
  src/ui_elements/slider.cpp
  src/ui_elements/input_text.cpp
  src/ui_elements/image.cpp
  src/ui_elements/separator.cpp
  src/ui_elements/menu.cpp
  src/ui_elements/tabs.cpp
  src/ui_elements/tree.cpp
  src/ui_elements/combo.cpp
  src/ui_elements/radio_button.cpp
  src/ui_elements/progress_bar.cpp
  src/ui_elements/input_number.cpp
  src/ui_elements/drag.cpp
  src/ui_elements/selectable.cpp
  src/ui_elements/docking.cpp
  src/ui_elements/table.cpp
  src/ui_elements/text_editor.cpp
  src/plot_elements/plot.cpp
  src/plot_elements/plot_series.cpp
  src/plot_elements/plot_bars.cpp
  src/plot_elements/plot_histogram.cpp
  src/plot_elements/plot_heatmap.cpp
  src/plot_elements/plot_pie.cpp
  src/plot_elements/plot_annotations.cpp
  src/plot3d_elements/plot3d.cpp
  src/plot3d_elements/plot3d_series.cpp
  src/plot3d_elements/plot3d_surface.cpp
  src/plot3d_elements/plot3d_mesh.cpp
  src/plot3d_elements/plot3d_annotations.cpp
  src/forms/form.cpp
  src/forms/file_dialog.cpp
  src/forms/calculator.cpp
)

if(WISH_MODULE_DESKTOP)
  target_sources(wish_server PRIVATE
    src/forms/desktop/desktop_module.cpp
    src/forms/desktop/calculator.cpp
    src/forms/desktop/notepad.cpp
  )
  target_compile_definitions(wish_server PRIVATE WISH_MODULE_DESKTOP)
endif()

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
  )
  target_compile_definitions(wish_server PUBLIC WISH_IMGUI_ENABLED)
  # PUBLIC so that consumers who include imgui_renderer.hpp have imgui in path.
  target_link_libraries(wish_server PUBLIC imgui_core)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  target_sources(wish_server PRIVATE
    src/sdl3_renderer.hpp
    src/sdl3_renderer.cpp
  )
  target_compile_definitions(wish_server PUBLIC WISH_SDL3_ENABLED)
  target_link_libraries(wish_server PUBLIC SDL3::SDL3 SDL3_image::SDL3_image)
endif()

set_target_properties(wish_server PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(wish_server
    PRIVATE
        src
        src/resources
        # nlohmann/json and libyaml headers; only needed by src/ui_importer.cpp.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/json/single_include
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/yaml/include
    PUBLIC
        src
        # Exposes bison's C++ headers (src/bison/bison_object.hpp, etc.)
        # using the same include convention as bison's own examples and tests.
        # PUBLIC because wish's own public headers reference bison types.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
)

# bison is PUBLIC so consumers of wish_server also get bison's types and link deps.
target_link_libraries(wish_server PUBLIC bison)
# libyaml is needed by ui_importer.cpp; PRIVATE because it is an impl detail.
target_link_libraries(wish_server PRIVATE yaml)

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
  target_link_libraries(wish_client_dll PUBLIC wish_client)
  target_include_directories(wish_client_dll
      PRIVATE
          ${CMAKE_SOURCE_DIR}
          ${CMAKE_SOURCE_DIR}/extern/bison
          ${CMAKE_SOURCE_DIR}/extern/bison/src/rmi)
  set_target_properties(wish_client_dll PROPERTIES
      RUNTIME_OUTPUT_NAME wish_client      # wish_client.dll
      ARCHIVE_OUTPUT_NAME wish_client_dll  # wish_client_dll.lib — avoids collision with wish_client.lib
      OUTPUT_NAME         wish_client      # MSYS2/Linux fallback
      FOLDER              lib)
endif()

add_subdirectory(app)
add_subdirectory(examples)
