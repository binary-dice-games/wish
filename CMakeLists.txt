# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Wish
  VERSION 1.0
  LANGUAGES CXX)

option(WISH_BUILD_TESTS "Build the wish tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Disable bison's own test suite in this build; wish manages its own tests
# and adds GoogleTest directly from bison's extern/ directory.
set(PACKAGE_TESTS OFF CACHE BOOL "Build bison tests" FORCE)
add_subdirectory("extern/bison")

add_library(wish STATIC
  include/wish/wish.hpp
  src/wish.cpp
)

target_include_directories(wish
    PRIVATE
        src
        # Exposes bison's C++ headers (src/bison/bison.hpp, src/rmi/rmi.hpp, etc.)
        # using the same include convention as bison's own examples and tests.
        ${CMAKE_CURRENT_SOURCE_DIR}/extern/bison
    PUBLIC
        include
)

# bison is PUBLIC so consumers of wish also get bison's types and link deps.
target_link_libraries(wish PUBLIC bison)

if(WISH_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
