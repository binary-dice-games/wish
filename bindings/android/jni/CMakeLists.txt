# bindings/android/jni/CMakeLists.txt
#
# JNI glue backing the Java/Kotlin binding (bindings/android/wish-lib) --
# only configured when cross-compiling with the Android NDK's CMake
# toolchain file (see the `if(ANDROID)` guard around this add_subdirectory()
# in the root CMakeLists.txt). A thin wrapper over the already-built
# wish_client_dll shared library (linked, not recompiled), matching how
# bison's own bindings/android/jni/CMakeLists.txt wraps bison_abi -- the
# alternative (recompiling src/wish_client_c.cpp, which #includes bison's
# bison_c.cpp/rmi_c.cpp, directly into this target) would produce a second,
# independent copy of the bison/rmi/wish C ABI in the same process; any
# bison_handle/rmi_proxy_handle this JNI layer hands to Java must remain
# valid against the exact wish_client_dll instance that created it. See
# docs/bindings.md for the binding's API surface.

add_library(wish_jni SHARED
    jni_util.cpp
    wish_jni.cpp
    wish_rmi_jni.cpp
)

target_include_directories(wish_jni PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/extern/bison/include"
    "${CMAKE_CURRENT_LIST_DIR}"
)

# The NDK sysroot provides <jni.h> and liblog.so (__android_log_*) without
# needing find_package(JNI) -- that's a host-JDK-only mechanism.
target_link_libraries(wish_jni PRIVATE wish_client_dll log)
