// MIT License © 2025 Binary Dice Games
/**
 * @file wish_server_c.h
 * @brief C ABI for the wish server — hosts a session and renders the UI
 *        connected clients build, over the real `bdg::wish::server`
 *        implementation (the same one the `wish server` CLI uses).
 *
 * Unlike `wish_client_c.h` (client-only, no server API), this header lets
 * any C-compatible language host a real wish server: template
 * registration/instantiation gives each widget its own independently
 * addressable RMI proxy, and events work, because this wraps the actual
 * `bdg::wish::server` C++ class rather than the generic bison RMI server
 * primitives (`rmi_server_*` in `rmi_c.h`), which cannot do either from
 * outside C++.
 *
 * This is a *separate* shared library from `wish_client_dll`
 * (`wish_server_dll`, gated by the `WISH_BUILD_SERVER_SHARED` CMake option)
 * — it links the full `wish_server` static library (Dear ImGui, SDL3, and
 * the web renderer's embedded HTTP/WebSocket server), so it is deliberately
 * kept out of the always-on, lightweight `wish_client_dll`.
 *
 * All functions are thread-safe unless noted otherwise. Opaque handles are
 * heap-allocated by the library and freed with wish_server_destroy().
 *
 * ## Typical usage
 * ```c
 * int main(void) {
 *   wish_server_handle s = wish_server_tcp_create("127.0.0.1", 7070);
 *   wish_server_start(s, "sdl3", NULL);   // opens a real SDL3 window
 *   while (!wish_server_should_quit(s))
 *     ; // sleep, poll, etc.
 *   wish_server_stop(s);
 *   wish_server_destroy(s);
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
#ifdef WISH_SERVER_BUILDING_DLL
#define WISH_SERVER_API __declspec(dllexport)
#else
#define WISH_SERVER_API __declspec(dllimport)
#endif
#else
#define WISH_SERVER_API __attribute__((visibility("default")))
#endif

/* ── Opaque handle ────────────────────────────────────────────────────────── */

/** @brief Opaque wish server handle. Freed by wish_server_destroy(). */
typedef struct wish_server_handle_* wish_server_handle;

/* ── Error codes ──────────────────────────────────────────────────────────── */

/** @brief Return type for fallible API calls; reuses wish_client_c.h's codes. */
typedef int wish_server_error;

#define WISH_SERVER_OK 0 /**< Success.                                */
#define WISH_SERVER_ERR_NULL -1 /**< A required pointer argument was NULL.  */
#define WISH_SERVER_ERR_TRANSPORT -3 /**< Transport listen failed.         */
#define WISH_SERVER_ERR_EXCEPTION -4 /**< An internal C++ exception was thrown. */
#define WISH_SERVER_ERR_BAD_RENDERER -6 /**< Unknown renderer_kind, or the library \
                                            wasn't built with support for the requested one. */

/* ── Lifecycle ─────────────────────────────────────────────────────────────── */

/**
 * @brief Create a TCP socket server.
 *
 * Does not listen; call wish_server_start() to begin accepting connections.
 *
 * @param host  Bind address (e.g. "127.0.0.1", "0.0.0.0").
 * @param port  Port number (0-65535).
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_SERVER_API wish_server_handle wish_server_tcp_create(const char* host, uint16_t port);

/**
 * @brief Create a named-pipe server.
 *
 * Does not listen; call wish_server_start() to begin accepting connections.
 *
 * @param path  Platform-specific pipe path/name.
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_SERVER_API wish_server_handle wish_server_pipe_create(const char* path);

/**
 * @brief Create a TLS-secured TCP socket server.
 *
 * Does not listen; call wish_server_start() to begin accepting connections.
 * TLS material (`cert_file`/`cert_pem`, `key_file`/`key_pem`,
 * `key_password`, and optionally `client_auth`/`ca_file`/`ca_pem` for mutual
 * TLS) is supplied via wish_server_start()'s `params`, matching
 * `tls_socket_server_transport::start()` (see
 * `src/rmi/transport/tls_socket_transport.hpp` in bison).
 *
 * @param host  Bind address (e.g. "127.0.0.1", "0.0.0.0").
 * @param port  Port number (0-65535).
 * @return Non-null handle on success; NULL on allocation failure.
 */
WISH_SERVER_API wish_server_handle wish_server_tls_create(const char* host, uint16_t port);

/**
 * @brief Create a terminal (OSC-99 framed) server by spawning a child
 *        process attached to a new pseudo-terminal.
 *
 * The spawned child is expected to be a wish client process using
 * wish_client_term_create() (or an equivalent term-transport client) over
 * its own inherited stdio. Does not begin accepting the connection; call
 * wish_server_start() to do so. wish_server_should_quit() also returns
 * non-zero once the spawned child exits, in addition to any renderer close
 * signal.
 *
 * @param cmd  Command to exec in the child. NULL or empty spawns the
 *             operator's `$SHELL` (Linux/MSYS2, falling back to `/bin/sh`)
 *             or `cmd.exe` (Windows).
 * @return Non-null handle on success; NULL on spawn/pty allocation failure.
 */
WISH_SERVER_API wish_server_handle wish_server_term_create(const char* cmd);

/**
 * @brief Build the requested renderer, start the render loop, and begin
 *        accepting client connections.
 *
 * @param server        Handle from wish_server_tcp_create()/wish_server_pipe_create()/
 *                      wish_server_tls_create()/wish_server_term_create().
 * @param renderer_kind One of:
 *                      - "sdl3": a real SDL3 window (requires the library
 *                        was built with WISH_ENABLE_SDL3=ON).
 *                      - "web": the web/browser renderer, served over its
 *                        own embedded HTTP+WebSocket listener, independent
 *                        of the RMI transport above (requires
 *                        WISH_ENABLE_WEB=ON).
 *                      - "console": a lightweight text dump of the widget
 *                        tree to stdout as it's built/updated. No display
 *                        needed; intended for tests/CI, not as a real UI.
 * @param params        Optional bison_handle with renderer-specific fields
 *                      (all optional, matching the `wish server` CLI's own
 *                      flags/defaults): "title" (string, default "wish"),
 *                      "width"/"height" (int, default 1280x720), "font_size"
 *                      (int, default 16) for "sdl3"/"web"; "web_bind"
 *                      (string, default "127.0.0.1") and "web_port" (int,
 *                      default 8080) for "web" only. Ignored for "console".
 *                      Also forwarded unchanged to the transport's own
 *                      `start()` as listen params -- e.g. `cert_file`/
 *                      `key_file`/etc. for a server created with
 *                      wish_server_tls_create(); ignored by every other
 *                      transport. May be NULL to use every default.
 * @return WISH_SERVER_OK on success; WISH_SERVER_ERR_BAD_RENDERER for an
 *         unrecognized renderer_kind or one this library wasn't built with;
 *         WISH_SERVER_ERR_TRANSPORT if listening failed;
 *         WISH_SERVER_ERR_EXCEPTION for any other internal failure (see
 *         wish_server_last_error()).
 */
WISH_SERVER_API wish_server_error
wish_server_start(wish_server_handle server, const char* renderer_kind, bison_handle params);

/** @brief Stop the accept loop, render loop, and join all threads. */
WISH_SERVER_API wish_server_error wish_server_stop(wish_server_handle server);

/**
 * @brief Returns non-zero once the renderer signals it should close (e.g.
 *        the SDL3 window was closed), or -- for a server created with
 *        wish_server_term_create() -- once the spawned child process has
 *        exited. The web/console renderers never set this on their own;
 *        stop those with an explicit wish_server_stop().
 */
WISH_SERVER_API int wish_server_should_quit(wish_server_handle server);

/**
 * @brief Deprecated: prefer wish_server_set_log_level(). `verbose != 0` maps to
 *        log level "trace", `0` maps to "none".
 */
WISH_SERVER_API wish_server_error wish_server_set_verbose(wish_server_handle server, int verbose);

/**
 * @brief Set the server log verbosity. One of
 *        "none" | "fatal" | "error" | "warning" | "info" | "trace"
 *        (case-insensitive; "warn"/"debug" also accepted). Default "none".
 *
 * RMI request/response trace lines are produced only at "info" and above
 * (mirrored to stdout), and carry decoded payloads only at "trace". Lower
 * levels only raise the severity floor for client log messages.
 *
 * Must be called before wish_server_start(). Returns WISH_SERVER_ERR_EXCEPTION
 * for an unrecognised level or if the server is already started.
 */
WISH_SERVER_API wish_server_error wish_server_set_log_level(wish_server_handle server, const char* level);

/** @brief Stop (if still running) and free a server handle. */
WISH_SERVER_API void wish_server_destroy(wish_server_handle server);

/** @brief Returns the last error message set on this handle, or "" if none. */
WISH_SERVER_API const char* wish_server_last_error(wish_server_handle server);

#ifdef __cplusplus
}
#endif
