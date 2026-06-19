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
using proxy_map =
    std::unordered_map<std::string, bison::rmi::proxy::dynamic>;

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
 *     register_template("ui"_key, R"({"type":"Window","title":"Hello"})").get();
 *     auto nodes = instantiate_template("ui"_key).get();
 *   }
 * };
 * ```
 *
 * @note The helper methods require the server to have the wish protocol
 *       handlers registered (`__WishTemplate`, `__WishFS`).
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
   * @param descriptor JSON or YAML descriptor string.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered.
   */
  std::future<void> register_template(
      bison::key_t name, const std::string& descriptor);

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
   * @throws std::logic_error until the server-side `__WishFS` protocol is
   *         accessible to the client.
   */
  std::future<void> upload_file(
      const std::string& name, const std::string& data);

  /**
   * @brief Download a previously uploaded file from the server.
   * @param name Filename (no path separators or `..`).
   * @return Future resolved with the file contents.
   * @throws std::logic_error until the server-side `__WishFS` protocol is
   *         accessible to the client.
   */
  std::future<std::string> download_file(const std::string& name);

 protected:
  /**
   * @brief Called after `connect()` completes; subclass performs all UI
   *        interaction here.  May throw; `run()` calls `disconnect()` in
   *        either case.
   */
  virtual void on_session() = 0;

  /** @brief Instantiates the wish protocol proxies once the session is open. */
  void on_connect() override;

  /** @brief Releases wish protocol proxies when the session ends. */
  void on_disconnect() override;

 private:
  // Per-session proxy handles.  Populated by on_connect() before on_session()
  // is called; cleared by on_disconnect().
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> fs_proxy_;
};

}  // namespace bdg::wish
