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
    target_sources(wish_server PRIVATE ${server_sources})
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
    if(client_sources)
      list(APPEND WISH_APP_MODULE_SOURCES ${client_sources})
      list(APPEND WISH_APP_MODULE_DEFS ${option_name})
      set(WISH_APP_MODULE_SOURCES "${WISH_APP_MODULE_SOURCES}" PARENT_SCOPE)
      set(WISH_APP_MODULE_DEFS "${WISH_APP_MODULE_DEFS}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Renders src/wish_module_registry.cpp.in with the includes/calls collected
# from every wish_add_module() call so far, and adds the result to
# wish_server. Must be called once, after all wish_add_module() calls.
function(wish_generate_module_registry)
  configure_file(
      "${CMAKE_SOURCE_DIR}/src/wish_module_registry.cpp.in"
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp"
      @ONLY)
  target_sources(wish_server PRIVATE
      "${CMAKE_BINARY_DIR}/generated/wish_module_registry.cpp")
endfunction()
