# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Wish
  VERSION 1.0
  LANGUAGES CXX)

option(PACKAGE_TESTS "Build the tests" ON)
  
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(WISH_PUBLIC_HEADERS
  	"${CMAKE_CURRENT_LIST_DIR}/src/wish.hpp"
)

set(WISH_PRIVATE_SOURCES
	src/wish.cpp
)

include_directories(. include)

add_subdirectory("extern/bison")

add_library(wish STATIC
  ${WISH_PUBLIC_HEADERS}
  ${WISH_PRIVATE_SOURCES}
)

target_include_directories(wish
    PRIVATE
        src
    PUBLIC
        include
)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)