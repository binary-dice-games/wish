// MIT License © 2025 Binary Dice Games
/**
 * @file client.hpp
 * @brief wish::client — RMI client base class with wish-specific helpers.
 */
#pragma once

#include "src/rmi/client/client.hpp"
#include "src/rmi/client/proxy.hpp"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

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
 *     register_template("ui"_key, wish::import_descriptor_json(R"({"type":"Window","title":"Hello"})")).get();
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
   */
  void run();

  /**
   * @brief Register a named UI template on the server.
   * @param name       Template name key.
   * @param descriptor Generic UI hierarchy tree, as produced by
   *                   `wish::import_descriptor_json`/`import_descriptor_yaml`
   *                   (src/ui_descriptor.hpp) from JSON/YAML text, or built
   *                   by hand. The server resolves it into typed elements.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   */
  std::future<void> register_template(bison::key_t name, bison::dynamic descriptor);

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
   * @param name Filename (no path separators or `..`).
   * @param data File contents.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<void> upload_file(const std::string& name, const std::string& data);

  /**
   * @brief Download a previously uploaded file from the server.
   * @param name Filename (no path separators or `..`).
   * @return Future resolved with the file contents.
   * @throws std::logic_error until the server-side `__WishFileSystem` protocol is
   *         accessible to the client.
   */
  std::future<std::string> download_file(const std::string& name);

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
  // Per-session proxy handles.  Populated by on_connect() before on_session()
  // is called; cleared by on_disconnect().
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> fs_proxy_;
  std::optional<bison::rmi::proxy::dynamic> style_proxy_;
  std::optional<bison::rmi::proxy::dynamic> log_proxy_;
};

} // namespace bdg::wish
