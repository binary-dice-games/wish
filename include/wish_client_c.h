// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_c.h
 * @brief C ABI for the wish client — connects to a running wish server and
 *        drives UI templates from any C-compatible language or runtime.
 *
 * This header only adds wish-specific session/template operations. Proxies
 * are plain `rmi_proxy_handle` values (see `rmi_c.h`): use
 * `rmi_proxy_get()` / `rmi_proxy_set()` / `rmi_proxy_call()` /
 * `rmi_proxy_on_event()` / `rmi_proxy_release()` to interact with the
 * proxies returned below, and `bison_c.h` to build/read the `bison_handle`
 * field payloads they exchange.
 *
 * All functions are thread-safe unless noted otherwise.  Opaque handles are
 * heap-allocated by the library and freed with wish_client_destroy().
 *
 * ## Typical usage
 * ```c
 * static void on_clicked(bison_handle params, void* ud) {
 *   (void)params;
 *   wish_client_quit((wish_client_handle)ud);
 * }
 *
 * static void session(wish_client_handle c, void* ud) {
 *   wish_set_style_preset(c, "dark");
 *   wish_register_template(c, "ui", kMyDesc);
 *   rmi_proxy_handle root = wish_instantiate_template(c, "ui", "ui");
 *   rmi_proxy_handle btn = wish_proxy_get(c, "ui.ok_button");
 *   rmi_proxy_on_event(btn, bison_key("clicked"), on_clicked, c);
 *   wish_client_wait(c);
 *   rmi_proxy_release(btn);
 *   rmi_proxy_release(root);
 * }
 *
 * int main(void) {
 *   wish_client_handle c = wish_client_tcp_create("127.0.0.1", 7070);
 *   wish_client_run(c, session, NULL);
 *   wish_client_destroy(c);
 * }
 * ```
 */
#pragma once

#include "rmi_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Export macro ─────────────────────────────────────────────────────────── */
#if defined(_WIN32)
#ifdef WISH_CLIENT_BUILDING_DLL
#define WISH_API __declspec(dllexport)
#else
#define WISH_API __declspec(dllimport)
#endif
#else
#define WISH_API __attribute__((visibility("default")))
#endif

/* ── Opaque handles ───────────────────────────────────────────────────────── */

/** @brief Opaque wish client session handle.  Freed by wish_client_destroy(). */
typedef struct wish_client_handle_* wish_client_handle;

/* ── Key hashing ──────────────────────────────────────────────────────────── */

/** @brief Pre-hashed field name type; identical to bison_hash. */
typedef bison_hash wish_hash;

/**
 * @brief Compute the FNV-1a hash of a field name at runtime.
 *
 * Identical to `bison_key()`; provided so wish-only callers don't need to
 * reference `bison_c.h` directly.  Produces the same value as the C++
 * `"name"_key` user-defined literal.
 *
 * @param name  Null-terminated ASCII/UTF-8 field name.
 * @return      Hash value with MSB set (never zero for non-empty input).
 */
WISH_API wish_hash wish_key(const char* name);

/* ── Error codes ──────────────────────────────────────────────────────────── */

/** @brief Return type for fallible API calls. */
typedef int wish_error;

#define WISH_OK 0 /**< Success.                                */
#define WISH_ERR_NULL -1 /**< A required pointer argument was NULL.   */
#define WISH_ERR_NOT_FOUND -2 /**< Named proxy or resource not found.      */
#define WISH_ERR_TRANSPORT -3 /**< Transport connection failed.            */
#define WISH_ERR_EXCEPTION -4 /**< An internal C++ exception was thrown.   */

/* ── Callbacks ────────────────────────────────────────────────────────────── */

/**
 * @brief Session callback invoked once the connection is established.
 *
 * Called from the RMI worker thread.  The session remains active for as long
 * as this function blocks.  Call wish_client_wait() to suspend here until
 * wish_client_quit() is invoked (e.g. from an event handler registered via
 * rmi_proxy_on_event()).
 *
 * @param client   The active session handle.
 * @param userdata Pointer supplied to wish_client_run().
 */
typedef void (*wish_session_fn)(wish_client_handle client, void* userdata);

/* ── Client lifecycle ─────────────────────────────────────────────────────── */

/**
 * @brief Create a TCP socket client.
 *
 * Does not connect; call wish_client_run() to establish the session.
 *
 * @param host  Server hostname or IP address (e.g., "127.0.0.1").
 * @param port  Server port number (0-65535).
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_API wish_client_handle wish_client_tcp_create(const char* host, uint16_t port);

/**
 * @brief Create a std::iostream-backed stream (FIFO / named pipe) client.
 *
 * Does not connect; call wish_client_run() to establish the session.  Linux
 * only.
 *
 * @param path  Filesystem path to a pre-existing FIFO.
 * @return Non-null handle on success; NULL if the FIFO cannot be opened.
 */
WISH_API wish_client_handle wish_client_stream_create(const char* path);

/**
 * @brief Create a named-pipe / Unix-socket client.
 *
 * Does not connect; call wish_client_run() to establish the session.
 *
 * On Windows/MSYS2, @p path is a full pipe path (`\\.\pipe\name`). On Linux,
 * @p path is a file-system socket path (e.g. `/tmp/wish.sock`).
 *
 * @param path  Pipe or Unix-socket path to connect to.
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_API wish_client_handle wish_client_pipe_create(const char* path);

/**
 * @brief Create a terminal (OSC-99 framed) client.
 *
 * Wraps the calling process's own inherited stdio (fd 0 for reads, fd 1 for
 * writes) with the term transport.  Intended for a client process that is
 * itself running as the child spawned by a wish server started with the
 * term transport.  Does not connect; call wish_client_run() to establish
 * the session.
 *
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_API wish_client_handle wish_client_term_create(void);

/**
 * @brief Destroy a wish client and free all associated resources.
 *
 * Must not be called while a session started by wish_client_run() is active.
 *
 * @param client  Handle to destroy; NULL is a no-op.
 */
WISH_API void wish_client_destroy(wish_client_handle client);

/**
 * @brief Connect, invoke the session callback, then disconnect.
 *
 * Blocks until the session callback returns (or until the connection drops).
 * The session callback runs on the RMI worker thread; call wish_client_wait()
 * inside it if you want to keep the session alive while event handlers run.
 *
 * @param client      Client handle from wish_client_tcp_create() (or another
 *                    wish_client_*_create() constructor).
 * @param session_fn  Session callback (must not be NULL).
 * @param userdata    Forwarded to session_fn unchanged.
 * @return WISH_OK on clean exit; WISH_ERR_* on transport or protocol failure.
 */
WISH_API wish_error wish_client_run(wish_client_handle client, wish_session_fn session_fn, void* userdata);

/**
 * @brief Connect with extra params, invoke the session callback, then disconnect.
 *
 * Identical to wish_client_run(), except @p connect_params is forwarded to
 * the underlying `wish::client::run(bison::dynamic)` -- it reaches both the
 * transport's connection setup and the server's connect handshake payload,
 * e.g. fields a server-side auth module inspects (see `src/auth/DESIGN.md`).
 * wish_client_run() is a thin wrapper around this function with an empty
 * @p connect_params.
 *
 * @param client         Client handle from wish_client_tcp_create() (or
 *                       another wish_client_*_create() constructor).
 * @param session_fn     Session callback (must not be NULL).
 * @param userdata       Forwarded to session_fn unchanged.
 * @param connect_params Optional connect params (`bison_handle` or `NULL`).
 * @return WISH_OK on clean exit; WISH_ERR_* on transport or protocol failure.
 */
WISH_API wish_error wish_client_run_with_params(
    wish_client_handle client, wish_session_fn session_fn, void* userdata, bison_handle connect_params);

/**
 * @brief Block inside the session callback until wish_client_quit() is called.
 *
 * Call this at the end of the session callback to keep the session alive while
 * event handlers update the UI.  Returns immediately if wish_client_quit()
 * was already called.
 *
 * @param client  Active session handle.
 */
WISH_API void wish_client_wait(wish_client_handle client);

/**
 * @brief Signal the session to end.
 *
 * Safe to call from any thread, including event callbacks.  After this call
 * returns, wish_client_wait() will unblock and the session callback will
 * return, causing wish_client_run() to disconnect and return.
 *
 * @param client  Active session handle.
 */
WISH_API void wish_client_quit(wish_client_handle client);

/**
 * @brief Retrieve the last error message for this client.
 *
 * @param client  Client handle.
 * @return Null-terminated string (never NULL); empty string if no error.
 */
WISH_API const char* wish_last_error(wish_client_handle client);

/* ── Style ────────────────────────────────────────────────────────────────── */

/**
 * @brief Apply a named built-in style preset for this session.
 *
 * @param client  Active session handle.
 * @param preset  "dark", "light", or "classic".
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_set_style_preset(wish_client_handle client, const char* preset);

/**
 * @brief Asynchronous variant of wish_set_style_preset().
 *
 * @param client      Active session handle.
 * @param preset      "dark", "light", or "classic".
 * @param out_future  Output future; the resolved value carries no result, so
 *                     consume it with rmi_future_wait() and discard it with
 *                     rmi_future_release().
 * @return WISH_OK if the operation was submitted; WISH_ERR_* otherwise.
 */
WISH_API wish_error
wish_set_style_preset_async(wish_client_handle client, const char* preset, rmi_future_handle* out_future);

/* ── Template management ──────────────────────────────────────────────────── */

/**
 * @brief Register a named UI template on the connected server.
 *
 * @param client      Active session handle.
 * @param name        Template name (ASCII, no spaces).
 * @param descriptor  JSON or YAML descriptor string.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_register_template(wish_client_handle client, const char* name, const char* descriptor);

/**
 * @brief Asynchronous variant of wish_register_template().
 *
 * @param out_future  Output future; the resolved value carries no result, so
 *                     consume it with rmi_future_wait() and discard it with
 *                     rmi_future_release().
 * @return WISH_OK if the operation was submitted; WISH_ERR_* otherwise.
 */
WISH_API wish_error wish_register_template_async(
    wish_client_handle client, const char* name, const char* descriptor, rmi_future_handle* out_future);

/**
 * @brief Instantiate a previously registered template.
 *
 * The result is merged into the internal dot-path → proxy map used by
 * wish_proxy_get() under @p prefix: the template's root lands at dot-path
 * @p prefix and its descendants at `prefix.<path>` (mirroring how
 * `bdg::wish::form` merges its own elements into the session's object tree
 * under a caller-chosen, per-instance key). Other prefixes already in the
 * map remain queryable; instantiating again under the same @p prefix only
 * replaces that instance's own subtree. Pass a distinct @p prefix (e.g. one
 * derived from a serial number) to instantiate multiple independent copies
 * of the same @p name.
 *
 * @param client  Active session handle.
 * @param name    Template name passed to wish_register_template().
 * @param prefix  Non-empty dot-path prefix under which this instance's
 *                elements are registered; caller-chosen and must be unique
 *                among the client's live instances to avoid colliding with
 *                another one's subtree.
 * @return Root proxy handle (dot-path @p prefix), or NULL on failure.
 *         Caller owns the handle and must release it with
 *         rmi_proxy_release().
 */
WISH_API rmi_proxy_handle wish_instantiate_template(wish_client_handle client, const char* name, const char* prefix);

/**
 * @brief Asynchronous variant of wish_instantiate_template().
 *
 * Resolves like `rmi_client_instantiate_async()`: consume @p out_future with
 * `rmi_future_get_proxy()` to obtain the root proxy handle (dot-path
 * @p prefix).  The dot-path → proxy map used by wish_proxy_get() is merged
 * under @p prefix as a side effect of the future becoming ready — see
 * wish_instantiate_template() for the prefixing/merge behavior.
 *
 * @param client      Active session handle.
 * @param name        Template name passed to wish_register_template().
 * @param prefix      Non-empty dot-path prefix for this instance; see
 *                    wish_instantiate_template().
 * @param out_future  Output future consumed with rmi_future_get_proxy().
 * @return WISH_OK if the operation was submitted; WISH_ERR_* otherwise.
 */
WISH_API wish_error wish_instantiate_template_async(
    wish_client_handle client, const char* name, const char* prefix, rmi_future_handle* out_future);

/**
 * @brief Resolve a dot-joined element path to a proxy handle.
 *
 * Paths are prefixed by the instance prefix passed to
 * wish_instantiate_template(), e.g. `wish_proxy_get(client, "ui.btns.ok")`
 * for the "ok" child of "btns" in the instance created with prefix "ui";
 * `wish_proxy_get(client, "ui")` for that instance's root.  Looked up from
 * the map populated by every wish_instantiate_template() (or
 * wish_instantiate_template_async()) call so far — no round trip to the
 * server.
 *
 * @param client    Active session handle.
 * @param dot_path  Dot-joined element path, prefixed by its instance prefix.
 * @return New proxy handle, or NULL if the path is not in the proxy map.
 *         Caller owns the handle and must release it with
 *         rmi_proxy_release().
 */
WISH_API rmi_proxy_handle wish_proxy_get(wish_client_handle client, const char* dot_path);

/**
 * @brief Release every proxy cached under @p prefix and its descendants.
 *
 * Removes @p prefix and every `prefix.<path>` entry merged in by a prior
 * wish_instantiate_template() (or its async variant) from the dot-path →
 * proxy map, freeing the underlying `rmi_proxy_handle` objects it owned.
 * Call this once the caller is done with an instantiated template — without
 * it, entries merged under distinct prefixes accumulate in the map for the
 * lifetime of the client. Proxy handles previously returned to the caller
 * (e.g. the root from wish_instantiate_template()) are independent copies
 * and are unaffected; release those separately with rmi_proxy_release().
 *
 * @param client  Active session handle.
 * @param prefix  Prefix passed to wish_instantiate_template() (or a
 *                dot-path within it) identifying the subtree to release.
 * @return WISH_OK, WISH_ERR_NULL, or WISH_ERR_NOT_FOUND if no entries
 *         matched @p prefix.
 */
WISH_API wish_error wish_release(wish_client_handle client, const char* prefix);

/* ── Object instantiation ─────────────────────────────────────────────────── */

/**
 * @brief Instantiate a remote object directly (no UI template involved).
 *
 * Mirrors `rmi_client_instantiate()` for a `wish_client_handle`, so callers
 * can create arbitrary registered classes on the server (e.g. an embedded
 * app's root widget, or an ad-hoc dialog such as `FileDialog`) without first
 * registering a template descriptor.  Unlike wish_instantiate_template(), the
 * result is not merged into the dot-path → proxy map used by
 * wish_proxy_get(); the caller keeps and releases the returned handle
 * directly.
 *
 * @param client  Active session handle.
 * @param ns      Namespace key to instantiate in, or `0` for global (use
 *                `wish_key()`).
 * @param klass   Class key to instantiate (use `wish_key()`).
 * @param params  Constructor parameters (`bison_handle` or `NULL`).
 * @return New proxy handle, or `NULL` on failure (see wish_last_error()).
 *         Caller owns the handle and must release it with
 *         `rmi_proxy_release()`.
 */
WISH_API rmi_proxy_handle
wish_instantiate(wish_client_handle client, wish_hash ns, wish_hash klass, bison_handle params);

/* ── File transfer ────────────────────────────────────────────────────────── */

/**
 * @brief Upload a file to the server's sandboxed session resource directory.
 *
 * @param client    Active session handle.
 * @param name      Filename (no path separators or `..`).
 * @param data      File contents; may contain embedded NUL bytes.
 * @param data_len  Length of @p data in bytes.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error
wish_upload_file(wish_client_handle client, const char* name, const char* data, size_t data_len);

/**
 * @brief Download a previously uploaded file from the server.
 *
 * @param client    Active session handle.
 * @param name      Filename (no path separators or `..`).
 * @param out_data  Output buffer, heap-allocated; may contain embedded NUL
 *                  bytes.  Release with `bison_free_string()`.
 * @param out_len   Output length of @p out_data in bytes.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error
wish_download_file(wish_client_handle client, const char* name, char** out_data, size_t* out_len);

/**
 * @brief Upload a file to the server, streaming it in chunks from a local
 *        file on disk instead of buffering the whole content in memory.
 *
 * Equivalent to `wish::client::upload_file(name, std::istream&)`; opens
 * @p local_path as a binary `std::ifstream` internally and streams it.
 *
 * @param client      Active session handle.
 * @param name        Destination filename on the server (no path separators
 *                    or `..`).
 * @param local_path  Path to a local file to read and upload.
 * @return WISH_OK or WISH_ERR_*; WISH_ERR_EXCEPTION if @p local_path cannot
 *         be opened.
 */
WISH_API wish_error wish_upload_file_from_path(wish_client_handle client, const char* name, const char* local_path);

/**
 * @brief Download a previously uploaded file, streaming it in chunks
 *        directly to a local file on disk instead of buffering the whole
 *        content in memory.
 *
 * Equivalent to `wish::client::download_file(name, std::ostream&)`; opens
 * @p local_path as a binary `std::ofstream` internally (truncating any
 * existing content) and streams into it.
 *
 * @param client      Active session handle.
 * @param name        Filename to download (no path separators or `..`).
 * @param local_path  Path to a local file to create/overwrite with the
 *                    downloaded content.
 * @return WISH_OK or WISH_ERR_*; WISH_ERR_EXCEPTION if @p local_path cannot
 *         be opened for writing.
 */
WISH_API wish_error wish_download_file_to_path(wish_client_handle client, const char* name, const char* local_path);

/**
 * @brief Upload a local zip archive and have the server unpack it into a
 *        sandboxed destination directory.
 *
 * Equivalent to `wish::client::upload_package(dest_path, std::istream&)`;
 * opens @p local_zip_path as a binary `std::ifstream` internally and
 * streams it.
 *
 * @param client          Active session handle.
 * @param dest_path       Destination directory, relative to the sandbox
 *                        (e.g. `"my_folder/my_package"`).
 * @param local_zip_path  Path to a local zip archive to upload and extract.
 * @return WISH_OK or WISH_ERR_*; WISH_ERR_EXCEPTION if @p local_zip_path
 *         cannot be opened, the archive is corrupt, or an entry would
 *         extract outside @p dest_path.
 */
WISH_API wish_error
wish_upload_package_from_path(wish_client_handle client, const char* dest_path, const char* local_zip_path);

/* ── Logging ──────────────────────────────────────────────────────────────── */

/**
 * @brief Send a structured log message to the server's logger service.
 *
 * The server writes the message to its global log file and, when started with
 * `--verbose`, also mirrors it to stdout.  The call is fire-and-forget.
 *
 * @param client  Active session handle.
 * @param level   Severity label: "debug", "info", "warn", or "error".
 * @param msg     Free-form message text (null-terminated UTF-8).
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log(wish_client_handle client, const char* level, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "debug", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_debug(wish_client_handle client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "info", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_info(wish_client_handle client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "warn", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_warn(wish_client_handle client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "error", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_error(wish_client_handle client, const char* msg);

#ifdef __cplusplus
}
#endif
