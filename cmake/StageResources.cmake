# cmake/StageResources.cmake
#
# Run via `cmake -P` from a build-time custom command (see
# wish_generate_embedded_resources() in cmake/WishResources.cmake). Reads
# WISH_MANIFEST (one "<abs_src_dir>|<prefix>" pair per line, blank prefix
# meaning "copy directly into the staging root") and copies each source
# directory's contents into WISH_STAGING_DIR/<prefix>/.
#
# Run via `cmake -P`, so this script never goes through the main project's
# cmake_minimum_required() and gets CMake's compiled-in OLD default for every
# policy, including CMP0007. Under OLD, list(GET parts 1 prefix) below fails
# ("index 1 out of range") for the top-level manifest line, which
# intentionally ends in a bare "|" (empty prefix): string(REPLACE "|" ";" ...)
# turns the trailing pipe into a trailing empty list element, and OLD's
# list() silently drops trailing empty elements, leaving parts with length 1.
# NEW keeps it, giving the 2-element list this script's list(GET ... 1 ...)
# calls assume.
cmake_policy(SET CMP0007 NEW)

file(MAKE_DIRECTORY "${WISH_STAGING_DIR}")
file(STRINGS "${WISH_MANIFEST}" lines)

foreach(line ${lines})
  if(line STREQUAL "")
    continue()
  endif()
  string(REPLACE "|" ";" parts "${line}")
  list(GET parts 0 src_dir)
  list(GET parts 1 prefix)
  if(EXISTS "${src_dir}")
    if(prefix STREQUAL "")
      file(COPY "${src_dir}/" DESTINATION "${WISH_STAGING_DIR}")
    else()
      file(COPY "${src_dir}/" DESTINATION "${WISH_STAGING_DIR}/${prefix}")
    endif()
  endif()
endforeach()
