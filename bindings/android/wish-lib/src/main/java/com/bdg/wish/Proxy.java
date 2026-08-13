// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

/**
 * A handle to a remote UI element or object, returned by {@link
 * Client#instantiateTemplate}, {@link Client#proxyGet}, or {@link
 * Client#instantiate}. Wraps {@code rmi_proxy_handle} (see {@code
 * extern/bison/include/rmi_c.h}).
 *
 * <p>{@link #set}/{@link #get}/{@link #clear}/{@link #call} are synchronous
 * with a timeout, matching {@code rmi_c.h}'s non-{@code _async} {@code
 * rmi_proxy_*} functions -- the async/{@code rmi_future_handle} half of the
 * C ABI is not yet exposed by this binding, matching bison's own Android
 * binding's documented gap. {@link #onEvent} (server-pushed events, e.g. a
 * button's {@code "clicked"} event) *is* exposed here, unlike bison's own
 * binding -- driving a UI template's interactivity is this binding's whole
 * point.
 */
public final class Proxy implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  /** Default timeout (`-1` = the internal RMI default), matching {@code rmi_c.h}. */
  public static final long DEFAULT_TIMEOUT_MS = -1;

  private long handle;

  Proxy(long handle) {
    this.handle = handle;
  }

  public void set(Dynamic fields) {
    set(fields, DEFAULT_TIMEOUT_MS);
  }

  public void set(Dynamic fields, long timeoutMs) {
    nativeSet(handle, fields == null ? 0 : fields.handle(), timeoutMs);
  }

  /** Retrieves a full snapshot of the remote object's fields. */
  public Dynamic get() {
    return get(null, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic get(Dynamic projection, long timeoutMs) {
    return Dynamic.wrapOwned(nativeGet(handle, projection == null ? 0 : projection.handle(), timeoutMs));
  }

  public void clear() {
    clear(DEFAULT_TIMEOUT_MS);
  }

  public void clear(long timeoutMs) {
    nativeClear(handle, timeoutMs);
  }

  public Dynamic call(String method, Dynamic args) {
    return call(method, args, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic call(String method, Dynamic args, long timeoutMs) {
    return Dynamic.wrapOwned(nativeCall(handle, Key.of(method), args == null ? 0 : args.handle(), timeoutMs));
  }

  /**
   * Subscribes {@code handler} to server-pushed event {@code eventName}
   * (e.g. a {@code Button}'s {@code "clicked"} event). There is no
   * unregister call -- matching {@code rmi_c.h}'s own contract -- so the
   * subscription lives as long as this proxy does.
   */
  public void onEvent(String eventName, EventHandler handler) {
    nativeOnEvent(handle, Key.of(eventName), handler);
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native void nativeSet(long handle, long fieldsHandle, long timeoutMs);
  private static native long nativeGet(long handle, long projectionHandle, long timeoutMs);
  private static native void nativeClear(long handle, long timeoutMs);
  private static native long nativeCall(long handle, int methodHash, long paramsHandle, long timeoutMs);
  private static native void nativeOnEvent(long handle, int eventNameHash, EventHandler handler);
  private static native void nativeRelease(long handle);
}
