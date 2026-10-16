# MIT License © 2026 Binary Dice Games
#
# FindFFMPEG
# ----------
# Finds the FFmpeg libraries wish's media playback decodes with:
# libavformat, libavcodec, libavutil, libswscale and libswresample.
#
# Uses pkg-config when available (Linux, macOS/Homebrew, MSYS2), otherwise
# searches FFMPEG_ROOT / WISH_FFMPEG_ROOT (e.g. an unpacked LGPL "shared"
# build or a vcpkg installed/<triplet> directory) for headers and import
# libraries -- the usual case on native Windows (MSVC).
#
# Result variables:
#   FFMPEG_FOUND    - all five libraries were found
#   FFMPEG_VERSION  - libavformat's version
# Imported target:
#   FFMPEG::FFMPEG  - link this to get every library and its include path

set(_ffmpeg_components avformat avcodec swscale swresample avutil)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_FFMPEG QUIET IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)
endif()

if(PC_FFMPEG_FOUND)
  set(FFMPEG_VERSION ${PC_FFMPEG_libavformat_VERSION})
  if(NOT TARGET FFMPEG::FFMPEG)
    add_library(FFMPEG::FFMPEG INTERFACE IMPORTED)
    target_link_libraries(FFMPEG::FFMPEG INTERFACE PkgConfig::PC_FFMPEG)
  endif()
  set(FFMPEG_FOUND TRUE)
else()
  set(_ffmpeg_hints ${FFMPEG_ROOT} ${WISH_FFMPEG_ROOT} $ENV{FFMPEG_ROOT})
  find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h HINTS ${_ffmpeg_hints} PATH_SUFFIXES include)
  set(_ffmpeg_libs)
  foreach(_c ${_ffmpeg_components})
    find_library(FFMPEG_${_c}_LIBRARY NAMES ${_c} HINTS ${_ffmpeg_hints} PATH_SUFFIXES lib)
    list(APPEND _ffmpeg_libs FFMPEG_${_c}_LIBRARY)
  endforeach()
  include(FindPackageHandleStandardArgs)
  find_package_handle_standard_args(FFMPEG REQUIRED_VARS FFMPEG_INCLUDE_DIR ${_ffmpeg_libs})
  if(FFMPEG_FOUND AND NOT TARGET FFMPEG::FFMPEG)
    add_library(FFMPEG::FFMPEG INTERFACE IMPORTED)
    target_include_directories(FFMPEG::FFMPEG INTERFACE ${FFMPEG_INCLUDE_DIR})
    foreach(_c ${_ffmpeg_components})
      target_link_libraries(FFMPEG::FFMPEG INTERFACE ${FFMPEG_${_c}_LIBRARY})
    endforeach()
    set(FFMPEG_VERSION "unknown")
  endif()
endif()
