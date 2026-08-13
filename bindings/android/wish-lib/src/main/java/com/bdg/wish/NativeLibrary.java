// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * Loads the native libraries backing this binding.
 *
 * <p>An Android app ships its native libraries inside the APK's {@code
 * jniLibs/<abi>/} directory and loads them by name with {@link
 * System#loadLibrary}. {@code wish_jni} (this binding's own JNI glue, built
 * from {@code bindings/android/jni/}) is linked against {@code
 * wish_client} (the {@code wish_client_dll} CMake target, which itself
 * embeds the bison and RMI C ABIs -- see {@code src/wish_client_c.cpp}), so
 * both must be loaded, {@code wish_client} first -- mirroring bison's own
 * {@code com.bdg.bison.NativeLibrary}. Deliberately does not depend on or
 * load bison's own {@code bison_abi}/{@code bison_jni}: every {@code
 * bison_handle}/{@code rmi_proxy_handle} this binding hands to Java is only
 * ever valid against the single wish_client_dll instance loaded here, and
 * loading a second, independent copy of the same C ABI would make it easy
 * to accidentally pass a handle across the two by mistake.
 */
public final class NativeLibrary {
  private static boolean loaded;

  /** Package-visibility escape hatch for classes in this package that need this too. */
  static synchronized void ensureLoaded() {
    if (loaded) return;
    System.loadLibrary("wish_client");
    System.loadLibrary("wish_jni");
    loaded = true;
  }

  private NativeLibrary() {}
}
