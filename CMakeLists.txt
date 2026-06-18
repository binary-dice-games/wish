# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Wish
  VERSION 1.0
  LANGUAGES CXX)

option(WISH_BUILD_TESTS "Build the wish tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Disable bison's own test suite in this build; wish manages its own tests
# and adds GoogleTest directly from bison's extern/ directory.
set(PACKAGE_TESTS OFF CACHE BOOL "Build bison tests" FORCE)
add_subdirectory("extern/bison")

add_library(wish STATIC
  include/wish/wish.hpp
  include/wish/registry.hpp
  include/wish/ui_importer.hpp
  include/wish/ui_element.hpp
  include/wish/session.hpp
  include/wish/renderer.hpp
  include/wish/file_service.hpp
  include/wish/server.hpp
  src/wish.cpp
  src/registry.cpp
  src/ui_importer.cpp
  src/ui_element.cpp
  src/session.cpp
  src/renderer.cpp
  src/file_service.cpp
  src/server.cpp
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
)

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

add_subdirectory(examples)
