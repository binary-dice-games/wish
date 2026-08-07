# cmake/WishModules.cmake
#
# wish_add_module(<org>/<collection>/<name>) wires up one optional module
# living in modules/<org>/<collection>/<name>/{server,client,resources/embedded}/
# (or, for an out-of-tree module, in whatever directory MODULE_DIR points at
# -- see "Out-of-tree modules" below). It declares an off-by-default
# WISH_MODULE_<ORG>_<COLLECTION>_<NAME> option and, when enabled:
#   - adds <module_dir>/server/*.{hpp,cpp} to wish_server and records
#     <module_dir>/server/<name>.hpp + register_<name>() for the generated
#     module registry (see wish_generate_module_registry() below). <name> is
#     the leaf (last path segment) -- so the header/function name never
#     includes the org/collection.
#   - adds <module_dir>/client/*.{hpp,cpp} (if present) to the
#     WISH_APP_MODULE_SOURCES / WISH_APP_MODULE_DEFS global properties,
#     consumed by app/CMakeLists.txt's embedded-app executables and
#     wish_client_dll. Client apps self-register into app_registry (see
#     modules/bdg/desktop/calculator/client/calculator.cpp for the pattern),
#     so no further wiring is needed here.
#   - adds <module_dir>/resources/embedded/ (if present) to the
#     WISH_MODULE_RESOURCE_DIRS global property, consumed by
#     wish_generate_embedded_resources() (see cmake/WishResources.cmake) to
#     fold the module's assets into the embedded resource archive under an
#     <org>/<collection>/<name>/ prefix.
#
# A module needs none, some, or all three of server/client/resources -- there
# is no assumption that any particular subdirectory exists.
#
# Adding a new in-tree module: create
# modules/<org>/<collection>/<name>/server/<name>.{hpp,cpp} (a
# register_<name>() free function) and/or
# modules/<org>/<collection>/<name>/client/<name>.{hpp,cpp} (a
# self-registering run_<name>()) and/or
# modules/<org>/<collection>/<name>/resources/embedded/, then either add one
# line to the root CMakeLists.txt -- wish_add_module(<org>/<collection>/<name>)
# -- or fold it into a wish_add_collection() call (see below). No edits to
# registry.cpp or app_registry.cpp are required. See modules/README.md.
#
# Collections
# -----------
# wish_add_collection(<org>/<collection> [DEFAULT ON|OFF] [MODULES <name1> <name2> ...])
# declares a WISH_COLLECTION_<ORG>_<COLLECTION> option (off by default,
# unless DEFAULT ON is passed) and calls wish_add_module() for every module
# in the collection, using that option's value as each module's DEFAULT --
# so enabling the collection pre-enables every module in it, while each
# module's own WISH_MODULE_<ORG>_<COLLECTION>_<NAME> option can still be
# individually overridden (e.g. -DWISH_COLLECTION_BDG_DESKTOP=ON
# -DWISH_MODULE_BDG_DESKTOP_NOTEPAD=OFF enables everything in bdg/desktop
# except notepad). If MODULES isn't given, every subdirectory of
# modules/<org>/<collection>/ that looks like a module (has server/, client/,
# or resources/embedded/) is discovered automatically.
#
# Out-of-tree modules
# --------------------
# A 3rd-party project that depends on wish (e.g. via add_subdirectory() or
# FetchContent, keeping its module's source in its own repo) can register
# its own module without editing anything inside the wish source tree:
#
#   add_subdirectory(extern/wish)
#   wish_add_module(acme/tools/mymodule MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mymodule)
#   wish_generate_module_registry()
#   wish_finalize_app_modules()
#   wish_generate_embedded_resources()
#
# A 3rd-party module still declares a full <org>/<collection>/<name> path
# (with MODULE_DIR pointing at wherever it actually lives) so its CMake
# option name and embedded-resource entry prefix follow the same convention
# and don't collide with wish's own modules.
#
# wish's own root CMakeLists.txt only auto-calls
# wish_generate_module_registry(), wish_finalize_app_modules(), and
# wish_generate_embedded_resources() when wish is the top-level project (see
# the guards there); when wish is included via add_subdirectory(), the
# consuming project must call all three itself, once, after all
# wish_add_module()/wish_add_collection() calls (its own and wish's built-in
# ones) have run. Module registration data is tracked in GLOBAL properties
# rather than directory-scoped variables specifically so that calls made
# from the consuming project's CMakeLists.txt (a different directory scope
# than wish's own) still merge correctly with wish's built-in modules.
#
# Naming collisions
# ------------------
# The generated module registry calls register_<name>() using only the leaf
# module name, and a module's client app registers under whatever `.name` it
# chooses in register_app() (see app_registry.hpp) -- neither is
# automatically qualified by org/collection. Two different orgs shipping a
# same-leaf-name module (or the same app name) in the same build will
# collide (a link error, or one app_registry entry silently shadowing
# another). This is intentionally not mechanically prevented, to avoid
# forcing every module to use verbose fully-qualified C++ identifiers --
# 3rd-party modules should pick distinctive names to avoid it in practice.

# Captured at include()-time: CMAKE_CURRENT_LIST_DIR inside a function()
# reflects the *caller's* listfile directory, not this file's, so it can't
# be used directly inside wish_add_module()/wish_generate_module_registry().
# A CACHE variable (rather than a plain set()) is required so this is still
# visible when a consuming project (a different, parent directory scope)
# calls wish_add_module()/wish_generate_module_registry() itself -- see
# "Out-of-tree modules" above.
set(WISH_MODULES_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")

define_property(GLOBAL PROPERTY WISH_MODULE_REGISTRY_INCLUDES)
define_property(GLOBAL PROPERTY WISH_MODULE_REGISTRY_CALLS)
define_property(GLOBAL PROPERTY WISH_APP_MODULE_SOURCES)
define_property(GLOBAL PROPERTY WISH_APP_MODULE_DEFS)
define_property(GLOBAL PROPERTY WISH_MODULE_RESOURCE_DIRS)

# Detect current platform and known platform tags. Files with a
# recognized platform suffix (for example *_win.cpp or *_linux.cpp)
# will only be included when the suffix matches the current platform.
if(WIN32)
  set(WISH_PLATFORM "win")
  set(WISH_ALLOWED_TAGS win win32 windows)
elseif(APPLE)
  set(WISH_PLATFORM "mac")
  set(WISH_ALLOWED_TAGS mac macos darwin)
elseif(UNIX)
  set(WISH_PLATFORM "linux")
  set(WISH_ALLOWED_TAGS linux unix posix)
else()
  set(WISH_PLATFORM "")
  set(WISH_ALLOWED_TAGS "")
endif()

# All recognized tags (used to detect whether a filename is platform-specific)
set(WISH_KNOWN_TAGS win win32 windows linux unix posix mac macos darwin)

function(wish_filter_platform_sources in_list out_var)
  set(filtered "")
  foreach(f ${${in_list}})
    get_filename_component(fname ${f} NAME_WE)
    set(matched_tag "")
    foreach(tag ${WISH_KNOWN_TAGS})
      string(REGEX MATCH "_${tag}$" tagmatch ${fname})
      if(tagmatch)
        set(matched_tag ${tag})
        break()
      endif()
    endforeach()

    if(matched_tag)
      list(FIND WISH_ALLOWED_TAGS ${matched_tag} idx)
      if(NOT idx EQUAL -1)
        list(APPEND filtered ${f})
      else()
        # skip file: platform-specific but not for current platform
      endif()
    else()
      # no platform tag -> include for all platforms
      list(APPEND filtered ${f})
    endif()
  endforeach()
  set(${out_var} "${filtered}" PARENT_SCOPE)
endfunction()

function(wish_add_module name)
  cmake_parse_arguments(ARG "" "MODULE_DIR;DEFAULT" "" ${ARGN})

  string(REPLACE "/" "_" name_flat "${name}")
  string(TOUPPER "${name_flat}" name_upper)
  set(option_name "WISH_MODULE_${name_upper}")

  string(REPLACE "/" ";" name_parts "${name}")
  list(GET name_parts -1 leaf)

  list(LENGTH name_parts name_parts_len)
  if(name_parts_len GREATER_EQUAL 3)
    list(GET name_parts 0 module_org)
    list(GET name_parts 1 module_collection)
  else()
    set(module_org "")
    set(module_collection "")
  endif()

  if(ARG_MODULE_DIR)
    set(module_dir "${ARG_MODULE_DIR}")
  else()
    # Resolve relative to wish's own root (the directory containing
    # cmake/), not CMAKE_SOURCE_DIR -- this file may be include()'d while
    # wish is a subdirectory of a larger project, in which case
    # CMAKE_SOURCE_DIR points at that outer project instead.
    set(module_dir "${WISH_MODULES_CMAKE_DIR}/../modules/${name}")
  endif()

  if(NOT DEFINED ARG_DEFAULT)
    set(ARG_DEFAULT OFF)
  endif()

  option(${option_name} "Include the ${name} optional module" ${ARG_DEFAULT})
  if(NOT ${option_name})
    return()
  endif()

  set(server_dir "${module_dir}/server")
  if(EXISTS "${server_dir}/${leaf}.hpp")
    file(GLOB_RECURSE server_sources CONFIGURE_DEPENDS
        "${server_dir}/*.hpp" "${server_dir}/*.cpp")
    wish_filter_platform_sources(server_sources server_sources_filtered)
    if(server_sources_filtered)
      target_sources(wish_server PRIVATE ${server_sources_filtered})
    endif()
    target_compile_definitions(wish_server PUBLIC ${option_name})

    set_property(GLOBAL APPEND_STRING PROPERTY WISH_MODULE_REGISTRY_INCLUDES
        "#include \"${server_dir}/${leaf}.hpp\"\n")
    set_property(GLOBAL APPEND_STRING PROPERTY WISH_MODULE_REGISTRY_CALLS
        "  register_${leaf}();\n")
  endif()

  set(client_dir "${module_dir}/client")
  if(EXISTS "${client_dir}")
    file(GLOB_RECURSE client_sources CONFIGURE_DEPENDS
        "${client_dir}/*.hpp" "${client_dir}/*.cpp")
    wish_filter_platform_sources(client_sources client_sources_filtered)
    if(client_sources_filtered)
      set_property(GLOBAL APPEND PROPERTY WISH_APP_MODULE_SOURCES ${client_sources_filtered})

      # Per-module-qualified (not per-source) defines: source-file properties
      # like COMPILE_DEFINITIONS are directory-scoped in CMake, so setting
      # one here (while processing the root CMakeLists.txt) would be invisible
      # once app/CMakeLists.txt (a different directory) later compiles these
      # same files into wish-cli/wish-client/etc. Target-level defines don't
      # have that problem (targets are global), but several modules' client
      # sources share the same target, so the define name must be unique per
      # module -- hence qualified by ${option_name}. Each module's own
      # register_app() call reads its own ${option_name}_ORGANIZATION/
      # _COLLECTION macro to populate app_info's organization/collection
      # fields (see src/client/app_registry.hpp), shown by
      # `--list`/wish_list_apps() to disambiguate same-leaf-name apps.
      set_property(GLOBAL APPEND PROPERTY WISH_APP_MODULE_DEFS
          "${option_name}"
          "${option_name}_ORGANIZATION=\"${module_org}\""
          "${option_name}_COLLECTION=\"${module_collection}\"")
    endif()
  endif()

  set(resources_dir "${module_dir}/resources/embedded")
  if(EXISTS "${resources_dir}")
    set_property(GLOBAL APPEND PROPERTY WISH_MODULE_RESOURCE_DIRS "${name}|${resources_dir}")
  endif()
endfunction()

# Declares WISH_COLLECTION_<ORG>_<COLLECTION> (off by default, or on by
# default if DEFAULT ON is passed) and calls wish_add_module() for every
# module in the collection, using that option's value as each module's
# DEFAULT. See "Collections" above.
function(wish_add_collection path)
  cmake_parse_arguments(ARG "" "DEFAULT" "MODULES" ${ARGN})

  if(NOT DEFINED ARG_DEFAULT)
    set(ARG_DEFAULT OFF)
  endif()

  string(REPLACE "/" "_" path_flat "${path}")
  string(TOUPPER "${path_flat}" path_upper)
  set(option_name "WISH_COLLECTION_${path_upper}")
  option(${option_name} "Include every module in ${path}" ${ARG_DEFAULT})

  if(ARG_MODULES)
    set(names ${ARG_MODULES})
  else()
    set(collection_dir "${WISH_MODULES_CMAKE_DIR}/../modules/${path}")
    set(names "")
    if(EXISTS "${collection_dir}")
      file(GLOB children RELATIVE "${collection_dir}" "${collection_dir}/*")
      foreach(child ${children})
        set(child_dir "${collection_dir}/${child}")
        if(IS_DIRECTORY "${child_dir}" AND
           (EXISTS "${child_dir}/server" OR EXISTS "${child_dir}/client" OR
            EXISTS "${child_dir}/resources/embedded"))
          list(APPEND names "${child}")
        endif()
      endforeach()
    endif()
  endif()

  foreach(name ${names})
    wish_add_module("${path}/${name}" DEFAULT ${${option_name}})
  endforeach()
endfunction()

# Applies the client-side sources/defs collected from every wish_add_module()
# call so far to whichever of wish-cli / wish-standalone / wish-client /
# wish_client_dll exist. Must be called once, after all wish_add_module()
# calls -- including any issued by a consuming project after
# add_subdirectory(wish). This can't happen unconditionally inside
# app/CMakeLists.txt or the wish_client_dll target definition, because those
# run while wish is still being processed via add_subdirectory(), before a
# consuming project's own wish_add_module() calls (issued after
# add_subdirectory(wish) returns) have had a chance to populate the GLOBAL
# properties -- see "Out-of-tree modules" above.
function(wish_finalize_app_modules)
  get_property(sources GLOBAL PROPERTY WISH_APP_MODULE_SOURCES)
  get_property(defs GLOBAL PROPERTY WISH_APP_MODULE_DEFS)
  foreach(tgt wish-cli wish-standalone wish-client wish_client_dll)
    if(TARGET ${tgt})
      target_sources(${tgt} PRIVATE ${sources})
      target_compile_definitions(${tgt} PRIVATE ${defs})
      # uv_a (libuv) is already compiled as part of every wish build --
      # bison's RMI transports depend on it (extern/bison/CMakeLists.txt) --
      # but only linked PRIVATE into the `bison` target, so it isn't
      # reachable from module client code without this. Linking it here
      # costs nothing extra when no module happens to use it directly; it
      # only exposes symbols/headers already being built. See the `git`
      # module's client/git_process.hpp for the consumer (a non-interactive
      # uv_spawn-based subprocess helper -- bison's own bdg::bison::term::
      # terminal is pty-only and unsuitable for repeated one-shot command
      # capture, see that file's doc comment for why).
      if(TARGET uv_a)
        target_link_libraries(${tgt} PRIVATE uv_a)
      endif()
    endif()
  endforeach()
endfunction()

# Renders src/server/wish_module_registry.cpp.in with the includes/calls
# collected from every wish_add_module() call so far, and adds the result to
# wish_server. Must be called once, after all wish_add_module() calls --
# including any issued by a consuming project after add_subdirectory(wish)
# (see "Out-of-tree modules" above).
function(wish_generate_module_registry)
  get_property(WISH_MODULE_REGISTRY_INCLUDES GLOBAL PROPERTY WISH_MODULE_REGISTRY_INCLUDES)
  get_property(WISH_MODULE_REGISTRY_CALLS GLOBAL PROPERTY WISH_MODULE_REGISTRY_CALLS)
  configure_file(
      "${WISH_MODULES_CMAKE_DIR}/../src/server/wish_module_registry.cpp.in"
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp"
      @ONLY)
  target_sources(wish_server PRIVATE
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp")
endfunction()
