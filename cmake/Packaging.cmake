# cmake/Packaging.cmake
#
# Release-zip packaging: install() destinations for content that isn't a
# CMake build target (docs, license, binding sources/headers), plus the
# CPack ZIP config that turns the whole install tree into a distributable
# archive. Target-owned install() rules (executables, wish_client_dll,
# public C headers) live next to their targets in app/CMakeLists.txt and the
# root CMakeLists.txt -- this file only adds what has nowhere else to live.
#
# Produces a zip via:
#   cmake --build <build-dir> --target package
# or, equivalently, running `cpack` from inside <build-dir>. See
# scripts/package_release.py for a full configure+build+package+post-process
# orchestrator (it also bundles the compiled C# binding and, on
# MSYS2/Windows, the runtime DLLs CPack has no way to know about).

install(FILES README.md LICENSE DESTINATION . COMPONENT wish)
install(DIRECTORY docs DESTINATION . COMPONENT wish)

# bindings/cpp is header-only -- ships as source, no build step needed by
# consumers beyond #include-ing it and linking the installed wish_client_dll.
install(DIRECTORY bindings/cpp/include/wish_cpp DESTINATION bindings/cpp/include
    COMPONENT wish)
install(DIRECTORY bindings/cpp/examples DESTINATION bindings/cpp
    COMPONENT wish)

# bindings/python is a plain importable package (no setup.py/pyproject.toml
# in this repo -- see docs/bindings.md); ship the source as-is, minus stray
# bytecode caches.
install(DIRECTORY bindings/python/wish DESTINATION bindings/python
    COMPONENT wish
    PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY bindings/python/examples DESTINATION bindings/python
    COMPONENT wish
    PATTERN "__pycache__" EXCLUDE)

# bindings/python/wish/_native.py does `sys.path.insert(0, ".../extern/bison
# /bindings/python")` and `from bison import _native` at import time -- that
# sibling package lives outside bindings/python/ entirely, so it must be
# installed alongside wish/ explicitly or `import wish` fails with
# "ModuleNotFoundError: No module named 'bison'" the moment it's run outside
# the source tree (this used to only happen in scripts/package_release.py's
# post-processing, which meant the plain `cpack`/`--target package` path
# silently produced a broken zip -- installing it here means every
# packaging path gets it, including a bare `cpack -G ZIP`).
if(EXISTS "${CMAKE_SOURCE_DIR}/extern/bison/bindings/python/bison")
  install(DIRECTORY extern/bison/bindings/python/bison DESTINATION bindings/python
      COMPONENT wish
      PATTERN "__pycache__" EXCLUDE)
else()
  message(WARNING "extern/bison/bindings/python/bison not found -- the "
      "packaged bindings/python/wish will be missing its 'bison' import "
      "dependency. Did you run 'git submodule update --init --recursive'?")
endif()

# _native.py's default library search assumes the in-repo build/ layout,
# which a standalone zip doesn't have -- point consumers at WISH_LIB.
set(_wish_lib_name "libwish_client.so")
if(WIN32)
  set(_wish_lib_name "wish_client.dll")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/wish-release-python-README.md"
"# Using the Python binding from this release zip

\`wish/_native.py\` looks for the wish_client shared library relative to an
in-repo \`build/\` directory by default, which this standalone zip doesn't
have. Point it at the bundled library explicitly:

\`\`\`sh
export WISH_LIB=\"$(pwd)/bin/${_wish_lib_name}\"   # Linux/MSYS2
set WISH_LIB=%cd%\\bin\\${_wish_lib_name}          REM Windows cmd
\`\`\`

Then \`sys.path.insert(0, \"bindings/python\")\` (this directory contains
both \`wish/\` and the sibling \`bison/\` package it imports) and
\`import wish\`.
")
install(FILES "${CMAKE_BINARY_DIR}/wish-release-python-README.md"
    DESTINATION bindings/python
    RENAME README-RELEASE.md
    COMPONENT wish)
unset(_wish_lib_name)

# bindings/csharp/Wish/Wish.csproj references extern/bison's C# binding via
# a repo-relative ProjectReference that won't resolve outside the full
# source tree, so the source alone isn't consumable standalone. Ship it
# anyway as reference material; scripts/package_release.py additionally
# `dotnet publish`es compiled DLLs into bindings/csharp/lib/ for consumers
# who just want to reference the library. Examples have the same
# ProjectReference problem (they reference ../../Wish/Wish.csproj), so
# they're reference material too, not standalone-runnable from the zip.
install(DIRECTORY bindings/csharp/Wish DESTINATION bindings/csharp/src
    COMPONENT wish)
install(DIRECTORY bindings/csharp/examples DESTINATION bindings/csharp
    COMPONENT wish)

# PATH/library-path registration helpers, so `wish` and wish_client.dll /
# libwish_client.so are discoverable from anywhere in the filesystem after
# extracting the zip, not just from inside bin/. Two per platform: a
# session-only "wish-env" (source/dot-source it, no files touched) and an
# opt-in "install" that persists the change (shell rc file on Linux, HKCU
# user PATH on Windows -- never machine-wide, never run automatically here).
# See packaging/{unix,windows}/*'s own comments for the platform-specific
# reasoning (Windows' DLL search consults PATH; Linux's dynamic linker does
# not, hence LD_LIBRARY_PATH/WISH_LIB on top of PATH there).
if(WIN32)
  install(PROGRAMS
      packaging/windows/wish-env.ps1
      packaging/windows/wish-env.cmd
      packaging/windows/install.ps1
      DESTINATION .
      COMPONENT wish)
else()
  install(PROGRAMS
      packaging/unix/wish-env.sh
      packaging/unix/install.sh
      DESTINATION .
      COMPONENT wish)
endif()

# ── CPack ────────────────────────────────────────────────────────────────
#
# All of this project's own install() rules (above, and the target-owned
# ones in app/CMakeLists.txt and this file's sibling in the root
# CMakeLists.txt) are tagged COMPONENT wish. FetchContent-vendored
# dependencies (civetweb, libuv, googletest, SDL3, miniz, ...) bring their
# own install() rules in as an untagged/default component when their
# subdirectories are added -- restricting packaging to just the "wish"
# component keeps the release zip to wish's own files instead of every
# dependency's dev artifacts (pkg-config files, CMake package-config
# exports, static archives that were never even built in a packaging
# configure).

set(CPACK_COMPONENTS_ALL wish)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL_IN_ONE_PACKAGE ON)

set(CPACK_PACKAGE_NAME "wish")
set(CPACK_PACKAGE_VENDOR "Binary Dice Games")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "wish -- a remote UI framework built on bison")

# WISH_PACKAGE_VERSION lets a release workflow stamp the zip with a tag
# version (e.g. "1.2.3") independent of the CMake project() VERSION, which
# is a slow-moving API-compatibility marker, not a release number.
if(NOT WISH_PACKAGE_VERSION)
  set(WISH_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()
set(CPACK_PACKAGE_VERSION "${WISH_PACKAGE_VERSION}")

set(CPACK_PACKAGE_FILE_NAME
    "wish-${WISH_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_VERBATIM_VARIABLES TRUE)

include(CPack)
