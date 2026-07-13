# cmake/StageResources.cmake
#
# Run via `cmake -P` from a build-time custom command (see
# wish_generate_embedded_resources() in cmake/WishResources.cmake). Reads
# WISH_MANIFEST (one "<abs_src_dir>|<prefix>" pair per line, blank prefix
# meaning "copy directly into the staging root") and copies each source
# directory's contents into WISH_STAGING_DIR/<prefix>/.

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
