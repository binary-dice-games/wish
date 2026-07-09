# cmake/WishModules.cmake
#
# wish_add_module(<name>) wires up one optional module living in
# modules/<name>/{server,client}/. It declares an off-by-default
# WISH_MODULE_<NAME> option and, when enabled:
#   - adds modules/<name>/server/*.{hpp,cpp} to wish_server and records
#     modules/<name>/server/<name>.hpp + register_<name>() for the generated
#     module registry (see wish_generate_module_registry() below).
#   - adds modules/<name>/client/*.{hpp,cpp} (if present) to
#     WISH_APP_MODULE_SOURCES / WISH_APP_MODULE_DEFS, consumed by
#     app/CMakeLists.txt's embedded-app executables. Client apps
#     self-register into app_registry (see modules/calculator/client/
#     calculator.cpp for the pattern), so no further wiring is needed here.
#
# Adding a new module: create modules/<name>/server/<name>.{hpp,cpp} (a
# register_<name>() free function) and, optionally,
# modules/<name>/client/<name>.{hpp,cpp} (a self-registering run_<name>()),
# then add one line to the root CMakeLists.txt: wish_add_module(<name>).
# No edits to registry.cpp or app_registry.cpp are required.

set(WISH_APP_MODULE_SOURCES "")
set(WISH_APP_MODULE_DEFS "")
set(WISH_MODULE_REGISTRY_INCLUDES "")
set(WISH_MODULE_REGISTRY_CALLS "")

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
  string(TOUPPER "${name}" name_upper)
  set(option_name "WISH_MODULE_${name_upper}")
  set(module_dir "${CMAKE_SOURCE_DIR}/modules/${name}")

  option(${option_name} "Include the ${name} optional module" OFF)
  if(NOT ${option_name})
    return()
  endif()

  set(server_dir "${module_dir}/server")
  if(EXISTS "${server_dir}/${name}.hpp")
    file(GLOB_RECURSE server_sources CONFIGURE_DEPENDS
        "${server_dir}/*.hpp" "${server_dir}/*.cpp")
    wish_filter_platform_sources(server_sources server_sources_filtered)
    if(server_sources_filtered)
      target_sources(wish_server PRIVATE ${server_sources_filtered})
    endif()
    target_compile_definitions(wish_server PUBLIC ${option_name})

    string(APPEND WISH_MODULE_REGISTRY_INCLUDES
        "#include \"modules/${name}/server/${name}.hpp\"\n")
    string(APPEND WISH_MODULE_REGISTRY_CALLS
        "  register_${name}();\n")
    set(WISH_MODULE_REGISTRY_INCLUDES "${WISH_MODULE_REGISTRY_INCLUDES}" PARENT_SCOPE)
    set(WISH_MODULE_REGISTRY_CALLS "${WISH_MODULE_REGISTRY_CALLS}" PARENT_SCOPE)
  endif()

  set(client_dir "${module_dir}/client")
  if(EXISTS "${client_dir}")
    file(GLOB_RECURSE client_sources CONFIGURE_DEPENDS
        "${client_dir}/*.hpp" "${client_dir}/*.cpp")
    wish_filter_platform_sources(client_sources client_sources_filtered)
    if(client_sources_filtered)
      list(APPEND WISH_APP_MODULE_SOURCES ${client_sources_filtered})
      list(APPEND WISH_APP_MODULE_DEFS ${option_name})
      set(WISH_APP_MODULE_SOURCES "${WISH_APP_MODULE_SOURCES}" PARENT_SCOPE)
      set(WISH_APP_MODULE_DEFS "${WISH_APP_MODULE_DEFS}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Renders src/server/wish_module_registry.cpp.in with the includes/calls
# collected from every wish_add_module() call so far, and adds the result to
# wish_server. Must be called once, after all wish_add_module() calls.
function(wish_generate_module_registry)
  configure_file(
      "${CMAKE_SOURCE_DIR}/src/server/wish_module_registry.cpp.in"
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp"
      @ONLY)
  target_sources(wish_server PRIVATE
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp")
endfunction()
