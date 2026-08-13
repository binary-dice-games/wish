// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * A self-describing, heterogeneous object -- this binding's wrapper around
 * {@code bison_handle} (see {@code extern/bison/include/bison_c.h}), used
 * for template/method parameters, {@link Proxy#get} snapshots, and {@link
 * EventHandler} payloads.
 *
 * <p>Every {@code Dynamic} obtained from a constructor, {@link #fromJson},
 * {@link #deserialize}, {@link #copy}, {@link Proxy#get}, or {@link
 * Proxy#call} owns a reference and must be {@link #close}d (or used in a
 * try-with-resources block); C++'s RAII destructor becomes Java's {@link
 * AutoCloseable}, the same choice the C# binding makes with {@code
 * IDisposable}. Instances handed to an {@link EventHandler} are
 * <em>borrowed</em> and must not be closed -- see that interface's docs.
 *
 * <h2>Gaps versus bison's own Android binding</h2>
 * This is a wish <em>client</em> binding (like the C#/Python calculator and
 * notepad examples), never a class-registration host, so unlike {@code
 * com.bdg.bison.Dynamic} it has no {@code addMethod}/{@code addClass}/
 * {@code call}/{@code registerClass} -- servers register classes and
 * methods in C++; this binding only builds and reads field values.
 */
public final class Dynamic implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  private long handle;
  private final boolean owned;

  public Dynamic() {
    this(0);
  }

  /** Creates a new object tagged with the given class name (see {@code __class}). */
  public Dynamic(String className) {
    this(Key.of(className));
  }

  private Dynamic(int classNameHash) {
    this.handle = nativeCreate(classNameHash);
    this.owned = true;
  }

  private Dynamic(long handle, boolean owned) {
    this.handle = handle;
    this.owned = owned;
  }

  /**
   * Wraps a new, owned {@code bison_handle} returned by a native call; {@code 0} becomes
   * {@code null}. Binding-internal: used by {@link Proxy} and {@code wish_jni.cpp}; not meant
   * for application code.
   */
  static Dynamic wrapOwned(long handle) {
    return handle == 0 ? null : new Dynamic(handle, true);
  }

  /** Binding-internal, see {@link #wrapOwned}. Wraps a borrowed handle (an {@link
   *  EventHandler} callback's {@code params}); must not be closed by the caller. Called by
   *  {@code wish_rmi_jni.cpp}'s JNI upcall -- must stay {@code public static} for that
   *  reflective lookup even though application code has no other reason to call it directly. */
  public static Dynamic wrapBorrowed(long handle) {
    return handle == 0 ? null : new Dynamic(handle, false);
  }

  /** Binding-internal, see {@link #wrapOwned}. The raw {@code bison_handle}, as a pointer-width integer. */
  long handle() {
    return handle;
  }

  public static Dynamic fromJson(String json) {
    return wrapOwned(nativeFromJson(json));
  }

  public static Dynamic deserialize(byte[] data) {
    return wrapOwned(nativeDeserialize(data));
  }

  /** Deep-copies this object (`bison_clone`). */
  public Dynamic copy() {
    return wrapOwned(nativeClone(handle));
  }

  @Override
  public void close() {
    if (handle != 0 && owned) {
      nativeRelease(handle);
    }
    handle = 0;
  }

  // ─── Scalar fields ────────────────────────────────────────────────────

  public void setInt(String field, int value) {
    nativeSetInt(handle, Key.of(field), value);
  }

  public void setFloat(String field, float value) {
    nativeSetFloat(handle, Key.of(field), value);
  }

  public void setBool(String field, boolean value) {
    nativeSetBool(handle, Key.of(field), value);
  }

  public void setString(String field, String value) {
    nativeSetString(handle, Key.of(field), value);
  }

  /** Sets a field whose value is itself a hashed key (distinct from a plain int32 field). */
  public void setKey(String field, int valueHash) {
    nativeSetKey(handle, Key.of(field), valueHash);
  }

  public void setObject(String field, Dynamic value) {
    nativeSetObject(handle, Key.of(field), value == null ? 0 : value.handle);
  }

  public int getInt(String field) {
    return nativeGetInt(handle, Key.of(field));
  }

  public float getFloat(String field) {
    return nativeGetFloat(handle, Key.of(field));
  }

  public boolean getBool(String field) {
    return nativeGetBool(handle, Key.of(field));
  }

  public String getString(String field) {
    return nativeGetString(handle, Key.of(field));
  }

  public int getKey(String field) {
    return nativeGetKey(handle, Key.of(field));
  }

  public Dynamic getObject(String field) {
    return wrapOwned(nativeGetObject(handle, Key.of(field)));
  }

  // ─── Vector fields ────────────────────────────────────────────────────

  public void setVectorBool(String field, boolean[] values) {
    nativeSetVectorBool(handle, Key.of(field), values);
  }

  public void setVectorInt(String field, int[] values) {
    nativeSetVectorInt(handle, Key.of(field), values);
  }

  public void setVectorFloat(String field, float[] values) {
    nativeSetVectorFloat(handle, Key.of(field), values);
  }

  public void setVectorBytes(String field, byte[] values) {
    nativeSetVectorBytes(handle, Key.of(field), values);
  }

  public boolean[] getVectorBool(String field) {
    return nativeGetVectorBool(handle, Key.of(field));
  }

  public int[] getVectorInt(String field) {
    return nativeGetVectorInt(handle, Key.of(field));
  }

  public float[] getVectorFloat(String field) {
    return nativeGetVectorFloat(handle, Key.of(field));
  }

  public byte[] getVectorBytes(String field) {
    return nativeGetVectorBytes(handle, Key.of(field));
  }

  // ─── Serialization ────────────────────────────────────────────────────

  public int size() {
    return (int) nativeSize(handle);
  }

  /** Compact binary wire format (see {@code FORMAT.md}); round-trips with {@link #deserialize}. */
  public byte[] serialize() {
    return nativeSerialize(handle);
  }

  public String toJson() {
    return toJson(-1);
  }

  /** @param indent Pretty-print indent width, or {@code -1} for compact output. */
  public String toJson(int indent) {
    return nativeToJson(handle, indent);
  }

  // ─── Native methods (implemented in bindings/android/jni/wish_jni.cpp) ─

  private static native long nativeCreate(int classNameHash);
  private static native long nativeFromJson(String json);
  private static native long nativeDeserialize(byte[] data);
  private static native long nativeClone(long handle);
  private static native void nativeRelease(long handle);

  private static native void nativeSetInt(long handle, int nameHash, int value);
  private static native void nativeSetFloat(long handle, int nameHash, float value);
  private static native void nativeSetBool(long handle, int nameHash, boolean value);
  private static native void nativeSetString(long handle, int nameHash, String value);
  private static native void nativeSetKey(long handle, int nameHash, int valueHash);
  private static native void nativeSetObject(long handle, int nameHash, long valueHandle);

  private static native int nativeGetInt(long handle, int nameHash);
  private static native float nativeGetFloat(long handle, int nameHash);
  private static native boolean nativeGetBool(long handle, int nameHash);
  private static native String nativeGetString(long handle, int nameHash);
  private static native int nativeGetKey(long handle, int nameHash);
  private static native long nativeGetObject(long handle, int nameHash);

  private static native void nativeSetVectorBool(long handle, int nameHash, boolean[] values);
  private static native void nativeSetVectorInt(long handle, int nameHash, int[] values);
  private static native void nativeSetVectorFloat(long handle, int nameHash, float[] values);
  private static native void nativeSetVectorBytes(long handle, int nameHash, byte[] values);

  private static native boolean[] nativeGetVectorBool(long handle, int nameHash);
  private static native int[] nativeGetVectorInt(long handle, int nameHash);
  private static native float[] nativeGetVectorFloat(long handle, int nameHash);
  private static native byte[] nativeGetVectorBytes(long handle, int nameHash);

  private static native long nativeSize(long handle);
  private static native byte[] nativeSerialize(long handle);
  private static native String nativeToJson(long handle, int indent);
}
