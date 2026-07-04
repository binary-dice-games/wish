// MIT License © 2025 Binary Dice Games
/**
 * @file wish_client_c.h
 * @brief C ABI for the wish client — connects to a running wish server and
 *        drives UI templates from any C-compatible language or runtime.
 *
 * All functions are thread-safe unless noted otherwise.  Opaque handles are
 * heap-allocated by the library and freed with wish_client_destroy().
 *
 * ## Typical usage
 * ```c
 * static void session(wish_client_t c, void* ud) {
 *   wish_set_style_preset(c, "dark");
 *   wish_register_template(c, "ui", kMyDesc);
 *   wish_instantiate_template(c, "ui");
 *   wish_proxy_t btn = wish_proxy_get(c, "ok_button");
 *   wish_proxy_on_event(btn, "clicked", on_ok, c);
 *   wish_client_wait(c);
 * }
 *
 * int main(void) {
 *   wish_client_t c = wish_client_create(WISH_TRANSPORT_SOCKET, "127.0.0.1:7070");
 *   wish_client_run(c, session, NULL);
 *   wish_client_destroy(c);
 * }
 * ```
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

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
typedef struct wish_client_s* wish_client_t;

/**
 * @brief Non-owning handle to a remote UI element proxy.
 *
 * Valid for the lifetime of the session that produced it.  Do not free.
 */
typedef struct wish_proxy_s* wish_proxy_t;

/* ── Key hashing ──────────────────────────────────────────────────────────── */

/** @brief Pre-hashed field name type (matches bison FNV-1a hash_t). */
typedef uint32_t wish_hash;

/**
 * @brief Compute the FNV-1a hash of a field name at runtime.
 *
 * Produces the same value as the C++ `"name"_key` user-defined literal so
 * that hashes computed here can be passed to proxy field setters.
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

/* ── Transport selection ──────────────────────────────────────────────────── */

/**
 * @brief Transport type passed to wish_client_create().
 */
typedef enum {
  /** TCP socket.  `address` = "host:port", e.g. "127.0.0.1:7070". */
  WISH_TRANSPORT_SOCKET = 0,
  /**
   * std::iostream-backed stream (FIFO / named pipe).
   * `address` = filesystem path to a pre-existing FIFO.
   * Linux only.
   */
  WISH_TRANSPORT_STREAM = 1,
  /**
   * Unix domain socket (Linux and MSYS2).
   * `address` = socket path, e.g. `/tmp/wish.sock`.
   */
  WISH_TRANSPORT_PIPE = 2,
} wish_transport_t;

/* ── Callbacks ────────────────────────────────────────────────────────────── */

/**
 * @brief Session callback invoked once the connection is established.
 *
 * Called from the RMI worker thread.  The session remains active for as long
 * as this function blocks.  Call wish_client_wait() to suspend here until
 * wish_client_quit() is invoked (e.g. from an event handler).
 *
 * @param client   The active session handle.
 * @param userdata Pointer supplied to wish_client_run().
 */
typedef void (*wish_session_fn)(wish_client_t client, void* userdata);

/**
 * @brief Event callback invoked when a UI element fires an event.
 *
 * Called from the event dispatch thread.  Must not block.
 *
 * @param src      Handle to the proxy that emitted the event.
 * @param event    FNV-1a hash of the event name (e.g. wish_key("clicked")).
 * @param userdata Pointer supplied to wish_proxy_on_event().
 */
typedef void (*wish_event_fn)(wish_proxy_t src, wish_hash event, void* userdata);

/* ── Client lifecycle ─────────────────────────────────────────────────────── */

/**
 * @brief Create a wish client for the given transport.
 *
 * Does not connect; call wish_client_run() to establish the session.
 *
 * @param transport  Transport type.
 * @param address    Transport address string (semantics depend on transport).
 * @return Non-null handle on success; NULL if transport construction fails
 *         (e.g. unsupported transport on this platform, bad address format).
 */
WISH_API wish_client_t wish_client_create(wish_transport_t transport, const char* address);

/**
 * @brief Destroy a wish client and free all associated resources.
 *
 * Must not be called while a session started by wish_client_run() is active.
 *
 * @param client  Handle to destroy; NULL is a no-op.
 */
WISH_API void wish_client_destroy(wish_client_t client);

/**
 * @brief Connect, invoke the session callback, then disconnect.
 *
 * Blocks until the session callback returns (or until the connection drops).
 * The session callback runs on the RMI worker thread; call wish_client_wait()
 * inside it if you want to keep the session alive while event handlers run.
 *
 * @param client      Client handle from wish_client_create().
 * @param session_fn  Session callback (must not be NULL).
 * @param userdata    Forwarded to session_fn unchanged.
 * @return WISH_OK on clean exit; WISH_ERR_* on transport or protocol failure.
 */
WISH_API wish_error wish_client_run(wish_client_t client, wish_session_fn session_fn, void* userdata);

/**
 * @brief Block inside the session callback until wish_client_quit() is called.
 *
 * Call this at the end of the session callback to keep the session alive while
 * event handlers update the UI.  Returns immediately if wish_client_quit()
 * was already called.
 *
 * @param client  Active session handle.
 */
WISH_API void wish_client_wait(wish_client_t client);

/**
 * @brief Signal the session to end.
 *
 * Safe to call from any thread, including event callbacks.  After this call
 * returns, wish_client_wait() will unblock and the session callback will
 * return, causing wish_client_run() to disconnect and return.
 *
 * @param client  Active session handle.
 */
WISH_API void wish_client_quit(wish_client_t client);

/**
 * @brief Retrieve the last error message for this client.
 *
 * @param client  Client handle.
 * @return Null-terminated string (never NULL); empty string if no error.
 */
WISH_API const char* wish_last_error(wish_client_t client);

/* ── Style ────────────────────────────────────────────────────────────────── */

/**
 * @brief Apply a named built-in style preset for this session.
 *
 * @param client  Active session handle.
 * @param preset  "dark", "light", or "classic".
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_set_style_preset(wish_client_t client, const char* preset);

/* ── Template management ──────────────────────────────────────────────────── */

/**
 * @brief Register a named UI template on the connected server.
 *
 * @param client      Active session handle.
 * @param name        Template name (ASCII, no spaces).
 * @param descriptor  JSON or YAML descriptor string.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_register_template(wish_client_t client, const char* name, const char* descriptor);

/**
 * @brief Instantiate a previously registered template.
 *
 * Populates the internal proxy map used by wish_proxy_get().  Only one
 * template can be active at a time; calling this again replaces the previous
 * proxy map and invalidates all wish_proxy_t handles from the prior call.
 *
 * @param client  Active session handle.
 * @param name    Template name passed to wish_register_template().
 * @return Root proxy handle (key ""), or NULL on failure.
 */
WISH_API wish_proxy_t wish_instantiate_template(wish_client_t client, const char* name);

/**
 * @brief Resolve a dot-joined element path to a proxy handle.
 *
 * Paths follow the wish naming convention: children are joined with ".".
 * Example: "btns.ok" refers to the "ok" child of the "btns" element.
 *
 * @param client    Active session handle.
 * @param dot_path  Dot-joined element path, or "" for the root element.
 * @return Proxy handle on success; NULL if the path is not in the proxy map.
 */
WISH_API wish_proxy_t wish_proxy_get(wish_client_t client, const char* dot_path);

/* ── Proxy field setters ──────────────────────────────────────────────────── */

/**
 * @brief Set a string field on a remote UI element.
 *
 * @param proxy  Proxy handle from wish_proxy_get().
 * @param field  FNV-1a hash of the field name (use wish_key("name")).
 * @param value  New string value (UTF-8, null-terminated).
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_proxy_set_string(wish_proxy_t proxy, wish_hash field, const char* value);

/**
 * @brief Set an integer field on a remote UI element.
 *
 * @param proxy  Proxy handle.
 * @param field  FNV-1a hash of the field name.
 * @param value  New int32 value.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_proxy_set_int(wish_proxy_t proxy, wish_hash field, int32_t value);

/**
 * @brief Set a float field on a remote UI element.
 *
 * @param proxy  Proxy handle.
 * @param field  FNV-1a hash of the field name.
 * @param value  New float value.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_proxy_set_float(wish_proxy_t proxy, wish_hash field, float value);

/**
 * @brief Set a boolean field on a remote UI element.
 *
 * @param proxy  Proxy handle.
 * @param field  FNV-1a hash of the field name.
 * @param value  Non-zero for true; zero for false.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_proxy_set_bool(wish_proxy_t proxy, wish_hash field, int value);

/* ── Event subscription ───────────────────────────────────────────────────── */

/**
 * @brief Subscribe to an event emitted by a remote UI element.
 *
 * @param proxy     Proxy handle for the element to watch.
 * @param event     Event name (e.g. "clicked", "changed").
 * @param callback  Function called on the event dispatch thread; must not block.
 * @param userdata  Forwarded to callback unchanged.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_proxy_on_event(wish_proxy_t proxy, const char* event, wish_event_fn callback, void* userdata);

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
WISH_API wish_error wish_log(wish_client_t client, const char* level, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "debug", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_debug(wish_client_t client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "info", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_info(wish_client_t client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "warn", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_warn(wish_client_t client, const char* msg);

/**
 * @brief Convenience wrapper for wish_log(client, "error", msg).
 * @param client  Active session handle.
 * @param msg     Message text.
 * @return WISH_OK or WISH_ERR_*.
 */
WISH_API wish_error wish_log_error(wish_client_t client, const char* msg);

#ifdef __cplusplus
}
#endif
