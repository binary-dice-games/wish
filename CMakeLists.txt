# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Wish
  VERSION 1.0
  LANGUAGES CXX)

option(WISH_BUILD_TESTS   "Build the wish tests"                            ON)
option(WISH_ENABLE_IMGUI  "Build the imgui renderer"                       ON)
option(WISH_ENABLE_SDL3   "Build the SDL3 renderer and calculator example" ON)
option(WISH_BUILD_SHARED  "Build wish_client shared library with C ABI"   ON)

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
  FetchContent_MakeAvailable(imgui implot implot3d)

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
  )
  target_include_directories(imgui_core PUBLIC
    ${imgui_SOURCE_DIR}
    ${implot_SOURCE_DIR}
    ${implot3d_SOURCE_DIR}
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
  FetchContent_MakeAvailable(SDL3)

  # imgui docking branch ships SDL3 backend sources; add them to imgui_core.
  target_sources(imgui_core PRIVATE
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_include_directories(imgui_core PUBLIC ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui_core PUBLIC SDL3::SDL3)
endif()

add_library(wish STATIC
  include/wish/wish.hpp
  include/wish/registry.hpp
  include/wish/ui_importer.hpp
  include/wish/ui_element.hpp
  include/wish/session.hpp
  include/wish/renderer.hpp
  include/wish/file_service.hpp
  include/wish/style_service.hpp
  include/wish/server.hpp
  include/wish/client.hpp
  src/wish.cpp
  src/registry.cpp
  src/ui_importer.cpp
  src/ui_element.cpp
  src/session.cpp
  src/renderer.cpp
  src/file_service.cpp
  src/style_service.cpp
  src/server.cpp
  src/client.cpp
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
)

if(WISH_ENABLE_IMGUI)
  target_sources(wish PRIVATE
    include/wish/imgui_renderer.hpp
    src/imgui_renderer.cpp
    src/imgui_ui_renderer.hpp
    src/imgui_ui_renderer.cpp
    src/imgui_plot_renderer.hpp
    src/imgui_plot_renderer.cpp
    src/imgui_plot3d_renderer.hpp
    src/imgui_plot3d_renderer.cpp
  )
  target_compile_definitions(wish PUBLIC WISH_IMGUI_ENABLED)
  # PUBLIC so that consumers who include imgui_renderer.hpp have imgui in path.
  target_link_libraries(wish PUBLIC imgui_core)
endif()

if(WISH_ENABLE_IMGUI AND WISH_ENABLE_SDL3)
  target_sources(wish PRIVATE
    include/wish/sdl3_renderer.hpp
    src/sdl3_renderer.cpp
  )
  target_compile_definitions(wish PUBLIC WISH_SDL3_ENABLED)
  target_link_libraries(wish PUBLIC SDL3::SDL3)
endif()

target_include_directories(wish
    PRIVATE
        src
        # nlohmann/json and libyaml headers; only needed by src/ui_importer.cpp.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/json/single_include
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison/extern/yaml/include
    PUBLIC
        include
        # Exposes bison's C++ headers (src/bison/bison_object.hpp, etc.)
        # using the same include convention as bison's own examples and tests.
        # PUBLIC because wish's own public headers reference bison types.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
)

# bison is PUBLIC so consumers of wish also get bison's types and link deps.
target_link_libraries(wish PUBLIC bison)
# libyaml is needed by ui_importer.cpp; PRIVATE because it is an impl detail.
target_link_libraries(wish PRIVATE yaml)

if(WISH_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

# ── wish_client shared library (C ABI) ───────────────────────────────────────
if(WISH_BUILD_SHARED)
  add_library(wish_client SHARED
      src/wish_client_c.cpp
      include/wish/wish_client.h)
  target_compile_definitions(wish_client PRIVATE WISH_CLIENT_BUILDING_DLL)
  target_link_libraries(wish_client PUBLIC wish)
  target_include_directories(wish_client
      PUBLIC
          ${CMAKE_SOURCE_DIR}/include
      PRIVATE
          ${CMAKE_SOURCE_DIR}
          ${CMAKE_SOURCE_DIR}/extern/bison)
  set_target_properties(wish_client PROPERTIES FOLDER lib)
endif()

add_subdirectory(app)
add_subdirectory(examples)
