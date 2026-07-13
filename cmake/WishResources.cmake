# cmake/WishResources.cmake
#
# wish_generate_embedded_resources() packs resources/embedded/ plus every
# enabled module's resources/embedded/ (recorded into the
# WISH_MODULE_RESOURCE_DIRS global property by wish_add_module(), see
# cmake/WishModules.cmake) into one zip archive, then embeds it as a single
# byte array compiled into wish_server
# (src/resources/embedded_resources.cpp.in -> embedded_resources.cpp, via
# cmake/GenerateResource.cmake -- unchanged).
#
# Must be called once, after all wish_add_module()/wish_add_collection()
# calls -- same ordering requirement, and the same reason, as
# wish_generate_module_registry()/wish_finalize_app_modules() (see
# cmake/WishModules.cmake's "Out-of-tree modules" section): a consuming
# project's own module registrations (issued after add_subdirectory(wish)
# returns) must have already run so their resource dirs are recorded in
# WISH_MODULE_RESOURCE_DIRS.
#
# Mechanism: `cmake -E tar`'s entry names are relative to its single
# WORKING_DIRECTORY, so every source tree (the top-level resources/embedded/
# plus each enabled module's resources/embedded/) is first merged into one
# staging directory (${CMAKE_BINARY_DIR}/embedded_staging/) via
# cmake/StageResources.cmake, a module's files landing under
# <org>/<collection>/<name>/ -- so at runtime,
# context::populate_resource_dir() (which extracts the whole archive to
# resource_dir/"res", unchanged) naturally produces
# resource_dir/res/<org>/<collection>/<name>/... alongside the top-level
# resource_dir/res/icons/... etc., with no resource_store/context changes
# required.

set(WISH_RESOURCES_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

function(wish_generate_embedded_resources)
  set(resource_dir  "${WISH_RESOURCES_CMAKE_DIR}/../resources/embedded")
  set(staging_dir    "${CMAKE_BINARY_DIR}/embedded_staging")
  set(embedded_cpp_in "${WISH_RESOURCES_CMAKE_DIR}/../src/resources/embedded_resources.cpp.in")
  set(embedded_cpp    "${WISH_RESOURCES_CMAKE_DIR}/../src/resources/embedded_resources.cpp")
  set(embedded_zip    "${CMAKE_BINARY_DIR}/wish_embedded_resources.zip")
  set(stage_script     "${WISH_RESOURCES_CMAKE_DIR}/StageResources.cmake")
  set(manifest_file   "${CMAKE_BINARY_DIR}/embedded_resources_manifest.txt")

  get_property(module_resource_dirs GLOBAL PROPERTY WISH_MODULE_RESOURCE_DIRS)

  # CONFIGURE_DEPENDS on every source tree involved, so adding/removing a
  # file anywhere triggers a reconfigure (matches the prior single-tree
  # behavior).
  file(GLOB_RECURSE top_assets CONFIGURE_DEPENDS RELATIVE "${resource_dir}" "${resource_dir}/*")
  set(any_assets ${top_assets})
  set(full_asset_paths "")
  foreach(rel ${top_assets})
    list(APPEND full_asset_paths "${resource_dir}/${rel}")
  endforeach()

  set(manifest_content "${resource_dir}|")
  foreach(entry ${module_resource_dirs})
    string(REPLACE "|" ";" entry_parts "${entry}")
    list(GET entry_parts 0 prefix)
    list(GET entry_parts 1 abs_dir)
    file(GLOB_RECURSE mod_assets CONFIGURE_DEPENDS RELATIVE "${abs_dir}" "${abs_dir}/*")
    if(mod_assets)
      list(APPEND any_assets ${mod_assets})
      foreach(rel ${mod_assets})
        list(APPEND full_asset_paths "${abs_dir}/${rel}")
      endforeach()
    endif()
    string(APPEND manifest_content "\n${abs_dir}|${prefix}")
  endforeach()

  file(GENERATE OUTPUT "${manifest_file}" CONTENT "${manifest_content}")

  if(any_assets)
    # Step 1: stage every enabled resource tree (top-level + modules) into
    # one merged directory, each module's files under its <org>/<collection>/
    # <name>/ prefix (see StageResources.cmake), then zip the whole staging
    # tree -- a build-time custom command (not file(ARCHIVE_CREATE), which
    # runs at configure time) so it participates correctly in incremental-
    # build OUTPUT/DEPENDS tracking.
    add_custom_command(
        OUTPUT "${embedded_zip}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${staging_dir}"
        COMMAND ${CMAKE_COMMAND}
            -DWISH_MANIFEST=${manifest_file}
            -DWISH_STAGING_DIR=${staging_dir}
            -P "${stage_script}"
        COMMAND ${CMAKE_COMMAND} -E chdir "${staging_dir}"
            ${CMAKE_COMMAND} -E tar cf "${embedded_zip}" --format=zip -- .
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS ${full_asset_paths} "${manifest_file}" "${stage_script}"
        COMMENT "Staging + zipping embedded resources (top-level + enabled modules) -> ${embedded_zip}"
        VERBATIM
    )

    # Step 2: embed the zip's bytes as a single static byte array.
    add_custom_command(
        OUTPUT "${embedded_cpp}"
        COMMAND ${CMAKE_COMMAND}
            -DWISH_EMBEDDED_ZIP=${embedded_zip}
            -DTEMPLATE_FILE=${embedded_cpp_in}
            -DOUTPUT_FILE=${embedded_cpp}
            -P "${WISH_RESOURCES_CMAKE_DIR}/GenerateResource.cmake"
        DEPENDS "${embedded_zip}" "${embedded_cpp_in}"
        COMMENT "Embedding resource archive as a C++ byte array"
    )
  else()
    # No assets found anywhere -- emit a zero-length array directly at
    # configure time so the build still succeeds.
    set(CPP_CONTENT "0x00")
    configure_file("${embedded_cpp_in}" "${embedded_cpp}" @ONLY)
  endif()

  target_sources(wish_server PRIVATE "${embedded_cpp}")
endfunction()
