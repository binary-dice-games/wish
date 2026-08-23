// MIT License © 2025 Binary Dice Games
/**
 * @file client.hpp
 * @brief wish::client — RMI client base class with wish-specific helpers.
 */
#pragma once

#include <client/wish_app_host.hpp>

#include "src/rmi/client/client.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Client-side map from dot-path name to a remote proxy handle.
///
/// The root proxy is stored at key `""`.  Named descendants follow the same
/// dot-joined naming convention as `wish::name_map` on the server.
using proxy_map = std::unordered_map<std::string, bison::rmi::proxy::dynamic>;

/**
 * @brief Connects to a wish server and exposes wish-level operations.
 *
 * Inherits all RMI client behaviour from `bison::rmi::client` and adds:
 * - A `run()` template method: connects, calls `on_session()`, then
 *   disconnects cleanly even if `on_session()` throws.
 * - Wish-specific helpers (`register_template`, `instantiate_template`,
 *   `upload_file`, `download_file`) that delegate to the server-side wish
 *   RMI protocol objects.
 *
 * ## Usage
 *
 * Subclass and override `on_session()`:
 * ```cpp
 * class my_client : public wish::client {
 *  protected:
 *   void on_session() override {
 *     register_template_from_json("ui"_key, R"({"type":"Window","title":"Hello"})").get();
 *     auto nodes = instantiate_template("ui"_key).get();
 *   }
 * };
 * ```
 *
 * @note The helper methods require the server to have the wish protocol
 *       handlers registered (`__WishTemplate`, `__WishFileSystem`).
 */
class client : public bison::rmi::client {
 public:
  // Inherit all bison::rmi::client constructors, including the template
  // convenience constructor that wraps concrete transport types in a unique_ptr.
  using bison::rmi::client::client;

  /**
   * @brief Connect to the server, call `on_session()`, then disconnect.
   *
   * Disconnects even if `on_session()` throws; the exception is re-thrown
   * after `disconnect()` completes.
   *
   * @param connect_params Forwarded unchanged to `bison::rmi::client::connect()`
   *                        -- reaches both the transport's `open()` and the
   *                        server's `OP_CONNECT` payload, e.g. fields a
   *                        server-side `auth_module_iface` inspects (see
   *                        `src/auth/DESIGN.md`). Empty by default, matching
   *                        prior behavior.
   */
  void run(bison::dynamic connect_params = bison::dynamic{});

  /**
   * @brief Register a callback fired once this session's connection has
   *        ended, whether via a clean caller-initiated `disconnect()`/`run()`
   *        return or the server unexpectedly closing the connection.
   *
   * Intended for a caller blocked on its own completion signal (e.g. an
   * app waiting for a `"closed"` event that can now never arrive, since the
   * server that would have sent it is gone) to wake up instead of hanging
   * forever. Fires from `on_disconnect()`, which -- per
   * `bison::rmi::client::on_disconnect()`'s doc comment -- may run on the
   * RMI worker thread rather than the thread that initiated the session, so
   * the callback must be non-blocking and must not make RMI calls.
   */
  void set_on_disconnected(std::function<void()> cb) {
    on_disconnected_ = std::move(cb);
  }

  /**
   * @brief Register a named UI template on the server.
   * @param name       Template name key.
   * @param descriptor Generic UI hierarchy tree, as produced by
   *                   `wish::import_descriptor_json`/`import_descriptor_yaml`
   *                   (src/ui/ui_descriptor.hpp) from JSON/YAML text, or built
   *                   by hand. The server resolves it into typed elements.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   */
  std::future<void> register_template(bison::key_t name, bison::dynamic descriptor);

  /**
   * @brief Parse a JSON descriptor and register it as a named UI template.
   *
   * Convenience wrapper: `wish::import_descriptor_json(json)` followed by
   * `register_template(name, ...)`, for callers who don't need to build or
   * inspect the intermediate `bison::dynamic` tree themselves.
   *
   * @param name Template name key.
   * @param json UTF-8 JSON text representing a wish UI hierarchy.
   * @throws std::runtime_error on JSON parse error.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   */
  std::future<void> register_template_from_json(bison::key_t name, const std::string& json);

  /**
   * @brief Parse a YAML descriptor and register it as a named UI template.
   * @param name Template name key.
   * @param yaml UTF-8 YAML text representing a wish UI hierarchy.
   * @throws std::runtime_error on YAML parse error.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   * @see register_template_from_json
   */
  std::future<void> register_template_from_yaml(bison::key_t name, const std::string& yaml);

  /**
   * @brief Instantiate a previously registered template.
   * @param name Template name key.
   * @return Future resolved with a proxy map; the root is at key `""`.
   * @throws std::runtime_error if the template name was never registered.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   */
  std::future<proxy_map> instantiate_template(bison::key_t name);

  /**
   * @brief Upload a file to the server's sandboxed session resource directory.
   *
   * When @p on_progress is left default (empty), this is a single monolithic
   * RMI call. When set, the upload instead runs in `kDefaultFileChunkSize`
   * chunks (same RMI ops as the `std::istream&` overload below), invoking
   * @p on_progress after each chunk -- this lets a caller (e.g. an event
   * handler running on the RMI worker/dispatch thread) hand the actual
   * transfer off to a background thread and still receive incremental
   * progress, without ever holding a single blocking `.get()` for the whole
   * transfer on a thread that must stay responsive.
   *
   * @param name       Filename (no path separators or `..`).
   * @param data       File contents.
   * @param on_progress Invoked after each chunk with bytes sent so far and
   *                    the total (`data.size()`). Leave default to skip
   *                    progress reporting and use a single RMI call.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<void> upload_file(
      const std::string& name, const std::string& data, transfer_progress_callback on_progress = nullptr);

  /**
   * @brief Download a previously uploaded file from the server.
   *
   * @see upload_file for the chunked-vs-monolithic behavior controlled by
   *      @p on_progress.
   * @param name        Filename (no path separators or `..`).
   * @param on_progress Invoked after each chunk with bytes received so far
   *                    and the total file size. Leave default to skip
   *                    progress reporting and use a single RMI call.
   * @return Future resolved with the file contents.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<std::string> download_file(const std::string& name, transfer_progress_callback on_progress = nullptr);

  /// @brief Default chunk size used by the streaming `upload_file` /
  ///        `download_file` / `upload_package` overloads.
  static constexpr std::size_t kDefaultFileChunkSize = 1u << 20; // 1 MiB

  /**
   * @brief Upload a file to the server's sandboxed session resource
   *        directory, reading it from @p data in chunks so the full content
   *        is never buffered in memory at once.
   *
   * @param name       Filename (no path separators or `..`).
   * @param data       Stream to read the file content from. Must remain
   *                   valid until the returned future is ready -- it is
   *                   read from a background thread, not copied.
   * @param chunk_size Number of bytes read from @p data per RMI call.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<void>
  upload_file(const std::string& name, std::istream& data, std::size_t chunk_size = kDefaultFileChunkSize);

  /**
   * @brief Download a previously uploaded file from the server, writing it
   *        to @p out in chunks so the full content is never buffered in
   *        memory at once.
   *
   * @param name       Filename (no path separators or `..`).
   * @param out        Stream the downloaded bytes are written to. Must
   *                   remain valid until the returned future is ready.
   * @param chunk_size Number of bytes requested per RMI call.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<void>
  download_file(const std::string& name, std::ostream& out, std::size_t chunk_size = kDefaultFileChunkSize);

  /**
   * @brief Upload a zip archive and have the server unpack it into a
   *        sandboxed destination directory.
   *
   * Streams @p package_stream to the server in chunks (same mechanism as
   * the `upload_file(name, std::istream&)` overload), then asks the server
   * to extract it into @p dest_path (created if missing) inside the
   * session's resource directory. For example,
   * `upload_package("my_folder/my_package", zip_stream)` extracts into
   * `my_folder/my_package/` in the sandbox. Extraction merges into an
   * existing destination directory; entries that would extract outside
   * @p dest_path are rejected.
   *
   * @param dest_path      Destination directory, relative to the sandbox.
   * @param package_stream Zip archive content. Must remain valid until the
   *                       returned future is ready.
   * @param chunk_size     Number of bytes read from @p package_stream per
   *                       RMI call.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   * @throws std::runtime_error (via the resolved future) if the archive is
   *         corrupt, either path escapes the sandbox, or an entry would
   *         extract outside @p dest_path.
   */
  std::future<void> upload_package(
      const std::string& dest_path,
      std::istream& package_stream,
      std::size_t chunk_size = kDefaultFileChunkSize);

  /**
   * @brief List file names found directly under a subdirectory of the
   *        server's sandboxed session resource directory.
   * @param path  Subdirectory relative to the resource directory (e.g.
   *              `"res/icons"` for the built-in icon set). Empty (the
   *              default) lists the resource directory's own top-level
   *              contents. Not recursive.
   * @return Future resolved with the file names found, in filesystem
   *         iteration order.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   * @throws std::runtime_error (via the resolved future) if @p path escapes
   *         the sandbox.
   */
  std::future<std::vector<std::string>> list_files(const std::string& path = "");

  /**
   * @brief Apply a named built-in style preset for this session.
   * @param name  `"dark"`, `"light"`, or `"classic"`.
   *
   * Clears any per-field overrides set by `set_style`; the preset becomes the
   * new baseline.  Call `set_style` after this to add individual overrides.
   */
  std::future<void> set_style_preset(const std::string& name);

  /**
   * @brief Merge per-field style overrides into the session's active style.
   *
   * Accepts a flat dynamic where keys map to:
   * - `float`  — scalar style fields (e.g. `"window_rounding"_key`) or vec2
   *              components (`"item_spacing_x"_key`, `"item_spacing_y"_key`).
   * - `string` — `"#RRGGBBAA"` hex color for color fields
   *              (e.g. `"color_button"_key`).
   *
   * To reset a field, call `set_style_preset` first to restore the baseline.
   */
  std::future<void> set_style(bison::dynamic params);

  /**
   * @brief Retrieve the current session style as a flat dynamic.
   * @return Future resolved with the style field map.
   */
  std::future<bison::dynamic> get_style();

  // ── Logging helpers ───────────────────────────────────────────────────────────

  /**
   * @brief Send a structured log message to the server's logger service.
   * @param level  Severity label: `"debug"`, `"info"`, `"warn"`, or `"error"`.
   * @param msg    Free-form message text.
   *
   * The call is fire-and-forget (oneway); it does not wait for a response.
   */
  std::future<void> log(const std::string& level, const std::string& msg);

  /// @brief Convenience wrapper for `log("debug", msg)`.
  std::future<void> log_debug(const std::string& msg);

  /// @brief Convenience wrapper for `log("info", msg)`.
  std::future<void> log_info(const std::string& msg);

  /// @brief Convenience wrapper for `log("warn", msg)`.
  std::future<void> log_warn(const std::string& msg);

  /// @brief Convenience wrapper for `log("error", msg)`.
  std::future<void> log_error(const std::string& msg);

  // ── Automation helpers ────────────────────────────────────────────────────
  //
  // Native (ABI-driven) automation: drive/introspect the session's UI over
  // this same RMI connection, without a browser -- see
  // `src/automation/DESIGN.md`'s "Native (ABI-based) automation" section.
  // Only available when the server's active renderer implements
  // `automation::automation_backend` (currently only `sdl3_renderer`);
  // `automation_supported()` reports whether `on_connect()` resolved it.

  /// @brief True once `on_connect()` has resolved automation support for
  ///        this session. False for a server whose active renderer doesn't
  ///        implement `automation::automation_backend` (e.g. the web
  ///        renderer, which uses its own separate browser-based mechanism).
  bool automation_supported() const noexcept {
    return automation_proxy_.has_value();
  }

  /**
   * @brief Query the current widget tree/hit-test snapshot.
   * @param root  Dot-path filter restricting the snapshot to that node and
   *              its descendants; empty means the whole tree.
   * @return Future resolved with TREE_SNAPSHOT-shaped JSON text -- see
   *         `automation::build_tree_snapshot`'s doc comment for the schema.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<std::string> get_automation_tree(const std::string& root = "");

  /**
   * @brief Retrieve the session's buffered automation log entries.
   * @return Future resolved with LOG_EVENT-shaped JSON text -- see
   *         `automation::build_log_event`'s doc comment for the schema.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<std::string> get_automation_logs();

  /**
   * @brief Capture a screenshot of the next frame the server renders.
   * @return Future resolved with PNG-encoded image bytes.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<std::vector<uint8_t>> take_screenshot();

  /**
   * @brief Inject a synthetic mouse-move event.
   * @param x  Window-relative X coordinate.
   * @param y  Window-relative Y coordinate.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<void> inject_mouse_move(float x, float y);

  /**
   * @brief Inject a synthetic mouse-button press/release.
   * @param button  0 = left, 1 = right, 2 = middle.
   * @param down    True for press, false for release.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<void> inject_mouse_button(int button, bool down);

  /**
   * @brief Inject a synthetic key press/release.
   * @param keycode  Platform keycode (`SDL_Keycode` for `sdl3_renderer`).
   * @param down     True for press, false for release.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<void> inject_key(int keycode, bool down);

  /**
   * @brief Inject synthetic text input (e.g. for typing into an InputText).
   * @param utf8  UTF-8 encoded text.
   * @throws std::logic_error (via the resolved future) if `automation_supported()` is false.
   */
  std::future<void> inject_text(const std::string& utf8);

 protected:
  /**
   * @brief Called after `connect()` completes; subclass performs all UI
   *        interaction here.  May throw; `run()` calls `disconnect()` in
   *        either case.  Default: no-op.
   */
  virtual void on_session() {}

  /** @brief Instantiates the wish protocol proxies once the session is open. */
  void on_connect() override;

  /** @brief Releases wish protocol proxies when the session ends. */
  void on_disconnect() override;

 private:
  // Synchronous chunk-loop helpers backing the streaming upload_file /
  // download_file overloads; also reused directly by upload_package to
  // avoid a nested std::async when staging the archive upload. Run on
  // whichever thread calls them -- the public overloads wrap these in
  // std::async, matching every other client helper.
  void upload_stream_sync(const std::string& name, std::istream& data, std::size_t chunk_size);
  void download_stream_sync(const std::string& name, std::ostream& out, std::size_t chunk_size);

  // Progress-reporting variants backing the chunked upload_file/download_file
  // overloads above. @p total is the known upload size upfront; for
  // downloads the total comes back from the server on each download_chunk
  // response instead, so no total parameter is needed there.
  void upload_stream_sync(
      const std::string& name,
      std::istream& data,
      std::size_t chunk_size,
      std::uint64_t total,
      const transfer_progress_callback& on_progress);
  void download_stream_sync(
      const std::string& name, std::ostream& out, std::size_t chunk_size, const transfer_progress_callback& on_progress);

  // Per-session proxy handles.  Populated by on_connect() before on_session()
  // is called; cleared by on_disconnect().
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> fs_proxy_;
  std::optional<bison::rmi::proxy::dynamic> style_proxy_;
  std::optional<bison::rmi::proxy::dynamic> log_proxy_;
  // Resolved non-fatally in on_connect() -- unset (rather than on_connect()
  // throwing and breaking the whole connection) when the server's active
  // renderer doesn't support automation. See automation_supported().
  std::optional<bison::rmi::proxy::dynamic> automation_proxy_;

  // Set via set_on_disconnected(); invoked from on_disconnect().
  std::function<void()> on_disconnected_;
};

} // namespace bdg::wish
