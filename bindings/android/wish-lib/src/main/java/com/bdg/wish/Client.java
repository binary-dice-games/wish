// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.wish;

import java.util.List;

/**
 * RAII-style wrapper around {@code wish_client_handle} (see {@code
 * include/wish_client_c.h}). Construct via {@link #tcp} or {@link #tls},
 * then call {@link #run} to connect, drive the session, and disconnect --
 * mirroring {@code wish_client_run()}: the session callback runs on the
 * library's internal RMI worker thread and the call blocks until it
 * returns (or a concurrent {@link #quit} unblocks a {@link #waitForQuit}
 * inside it).
 *
 * <p>Only the TCP and TLS transports are exposed by this binding -- the
 * ones meaningful on Android, where the app process (unlike a desktop CLI)
 * has no inherited stdio/pty to hop over and no access to filesystem-path
 * Unix-domain sockets shared with another process. Named-pipe and terminal
 * ({@code --transport=term}) transports are intentionally not bound,
 * matching bison's own Android binding's choice for {@code
 * com.bdg.bison.rmi.Client}. File-transfer's local-path streaming variants
 * ({@code wish_upload_file_from_path}/{@code wish_download_file_to_path}/
 * {@code wish_upload_package_from_path}) and the native-automation
 * functions (desktop dev/test tooling for the SDL3 renderer) are also not
 * bound, for the same "not meaningful on this platform" reason.
 */
public final class Client implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  private long handle;

  private Client(long handle) {
    this.handle = handle;
  }

  public static Client tcp(String host, int port) {
    return new Client(nativeTcpCreate(host, port));
  }

  /**
   * Creates a TLS-secured TCP client (not yet connected). TLS
   * trust/identity material (`ca_pem`, `insecure_skip_verify`, `cert_pem`,
   * `key_pem`, `key_password`, `server_name`) is supplied via {@link #run}'s
   * {@code params}.
   */
  public static Client tls(String host, int port) {
    return new Client(nativeTlsCreate(host, port));
  }

  /** Frees the client. Must not be called while {@link #run} is active. */
  @Override
  public void close() {
    if (handle != 0) {
      nativeDestroy(handle);
      handle = 0;
    }
  }

  public String lastError() {
    return nativeLastError(handle);
  }

  // ─── Session lifecycle ────────────────────────────────────────────────

  /** Connects, invokes {@code sessionFn}, then disconnects. See {@link #run(SessionCallback, Dynamic)}. */
  public void run(SessionCallback sessionFn) {
    run(sessionFn, null);
  }

  /**
   * Connects, invokes {@code sessionFn}, then disconnects. Blocks until
   * {@code sessionFn} returns; it runs on the library's internal RMI worker
   * thread, not the calling thread -- call this from a background thread if
   * the caller needs to remain responsive (e.g. to handle a cancel action
   * that calls {@link #quit}).
   *
   * <p>{@code params} is forwarded to both the transport's connection setup
   * and the server's connect handshake payload, e.g. fields a server-side
   * auth module inspects (see {@code src/auth/DESIGN.md}).
   */
  public void run(SessionCallback sessionFn, Dynamic params) {
    int rc = nativeRunWithParams(handle, this, sessionFn, params == null ? 0 : params.handle());
    if (rc != 0) throw new WishException(rc, lastError());
  }

  /** Blocks until {@link #quit} is called (from any thread). */
  public void waitForQuit() {
    nativeWait(handle);
  }

  /** Signals the session to end; unblocks a concurrent {@link #waitForQuit}. */
  public void quit() {
    nativeQuit(handle);
  }

  // ─── Style ──────────────────────────────────────────────────────────────

  /** Applies a built-in style preset: "dark", "light", or "classic". */
  public void setStylePreset(String preset) {
    check(nativeSetStylePreset(handle, preset));
  }

  // ─── Template management ─────────────────────────────────────────────────

  /** Registers a named UI template (JSON or YAML descriptor string). */
  public void registerTemplate(String name, String descriptor) {
    check(nativeRegisterTemplate(handle, name, descriptor));
  }

  /**
   * Instantiates a registered template under dot-path {@code prefix} and
   * returns a {@link Proxy} to its root.
   */
  public Proxy instantiateTemplate(String name, String prefix) {
    long h = nativeInstantiateTemplate(handle, name, prefix);
    if (h == 0) throw new WishException(-4 /* WISH_ERR_EXCEPTION */, lastError());
    return new Proxy(h);
  }

  /**
   * Resolves a dot-joined element path (see {@link #instantiateTemplate})
   * to a {@link Proxy}, from the client's local proxy map.
   */
  public Proxy proxyGet(String dotPath) {
    long h = nativeProxyGet(handle, dotPath);
    if (h == 0) throw new WishException(-2 /* WISH_ERR_NOT_FOUND */, "proxyGet(" + dotPath + ")");
    return new Proxy(h);
  }

  /** Releases every proxy cached under {@code prefix} and its descendants. */
  public void release(String prefix) {
    check(nativeRelease(handle, prefix));
  }

  // ─── Object instantiation ─────────────────────────────────────────────────

  /**
   * Instantiates a remote object directly (no UI template involved).
   * Unlike {@link #instantiateTemplate}, the result is not merged into the
   * dot-path proxy map used by {@link #proxyGet}; the caller keeps and
   * releases the returned proxy directly.
   */
  public Proxy instantiate(String className, String nsName, Dynamic params) {
    int nsKey = nsName == null || nsName.isEmpty() ? 0 : Key.of(nsName);
    int classKey = Key.of(className);
    long h = nativeInstantiate(handle, nsKey, classKey, params == null ? 0 : params.handle());
    if (h == 0) throw new WishException(-4 /* WISH_ERR_EXCEPTION */, lastError());
    return new Proxy(h);
  }

  // ─── Embedded apps ────────────────────────────────────────────────────────

  /**
   * Lists every embedded app registered by an enabled optional module (see
   * {@code modules/README.md}). Mirrors {@code wish client --list}. Does
   * not require a connection -- app registration happens at library load
   * time, independent of any session.
   */
  public static List<AppInfo> listApps() {
    return AppInfo.parseList(nativeListApps());
  }

  /**
   * Connects, runs the named embedded app (see {@link #listApps}), blocks
   * until it signals completion, then disconnects. Mirrors {@code wish
   * client --run=<name> -- <args...>}.
   */
  public void runApp(String name, String[] args) {
    check(nativeRunApp(handle, name, args == null ? new String[0] : args));
  }

  // ─── File transfer ────────────────────────────────────────────────────────

  /** Uploads a file to the server's sandboxed session resource directory. */
  public void uploadFile(String name, byte[] data) {
    check(nativeUploadFile(handle, name, data));
  }

  /** Downloads a previously uploaded file from the server. */
  public byte[] downloadFile(String name) {
    return nativeDownloadFile(handle, name);
  }

  // ─── Logging ────────────────────────────────────────────────────────────

  /** Sends a structured log message; {@code level} is "debug"/"info"/"warn"/"error". */
  public void log(String level, String msg) {
    check(nativeLog(handle, level, msg));
  }

  public void logDebug(String msg) { log("debug", msg); }
  public void logInfo(String msg) { log("info", msg); }
  public void logWarn(String msg) { log("warn", msg); }
  public void logError(String msg) { log("error", msg); }

  private void check(int rc) {
    if (rc != 0) throw new WishException(rc, lastError());
  }

  private static native long nativeTcpCreate(String host, int port);
  private static native long nativeTlsCreate(String host, int port);
  private static native void nativeDestroy(long handle);
  private static native int nativeRunWithParams(long handle, Client clientObj, SessionCallback callback, long paramsHandle);
  private static native void nativeWait(long handle);
  private static native void nativeQuit(long handle);
  private static native String nativeLastError(long handle);

  private static native int nativeSetStylePreset(long handle, String preset);

  private static native int nativeRegisterTemplate(long handle, String name, String descriptor);
  private static native long nativeInstantiateTemplate(long handle, String name, String prefix);
  private static native long nativeProxyGet(long handle, String dotPath);
  private static native int nativeRelease(long handle, String prefix);

  private static native long nativeInstantiate(long handle, int nsHash, int classHash, long paramsHandle);

  private static native String nativeListApps();
  private static native int nativeRunApp(long handle, String appName, String[] args);

  private static native int nativeUploadFile(long handle, String name, byte[] data);
  private static native byte[] nativeDownloadFile(long handle, String name);

  private static native int nativeLog(long handle, String level, String msg);
}
