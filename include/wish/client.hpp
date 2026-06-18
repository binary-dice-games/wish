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
#include <mutex>
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
 * - Wish-specific helpers (`import_ui`, `register_template`,
 *   `instantiate_template`, `upload_file`, `download_file`) that delegate
 *   to the server-side wish RMI protocol objects.
 *
 * ## Usage
 *
 * Subclass and override `on_session()`:
 * ```cpp
 * class my_client : public wish::client {
 *  protected:
 *   void on_session() override {
 *     auto nodes = import_ui(R"({"type":"Window","title":"Hello"})").get();
 *   }
 * };
 * ```
 *
 * @note The helper methods (`import_ui`, `register_template`, etc.) require
 *       the server to have the wish protocol handlers registered
 *       (`__WishImport`, `__WishTemplate`, `__WishFS`).
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
   * @brief Parse @p descriptor on the server and return a proxy name map.
   * @param descriptor JSON or YAML UI descriptor string.
   * @return Future resolved with a map of dot-path name → proxy handle,
   *         with the root element at key `""`.
   * @throws std::logic_error until the server-side `__WishImport` handler
   *         is registered (Step 11).
   */
  std::future<proxy_map> import_ui(const std::string& descriptor);

  /**
   * @brief Register a named UI template on the server.
   * @param name       Template name key.
   * @param descriptor JSON or YAML descriptor string.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered (Step 11).
   */
  std::future<void> register_template(
      bison::key_t name, const std::string& descriptor);

  /**
   * @brief Instantiate a previously registered template.
   * @param name Template name key.
   * @return Future resolved with a proxy name map identical to `import_ui`
   *         on the same descriptor.
   * @throws std::logic_error until the server-side `__WishTemplate` handler
   *         is registered (Step 11).
   */
  std::future<proxy_map> instantiate_template(bison::key_t name);

  /**
   * @brief Upload a file to the server's sandboxed session resource directory.
   * @param name Filename (no path separators or `..`).
   * @param data File contents.
   * @throws std::logic_error until the server-side `__WishFS` protocol is
   *         accessible to the client (Step 11).
   */
  std::future<void> upload_file(
      const std::string& name, const std::string& data);

  /**
   * @brief Download a previously uploaded file from the server.
   * @param name Filename (no path separators or `..`).
   * @return Future resolved with the file contents.
   * @throws std::logic_error until the server-side `__WishFS` protocol is
   *         accessible to the client (Step 11).
   */
  std::future<std::string> download_file(const std::string& name);

 protected:
  /**
   * @brief Called after `connect()` completes; subclass performs all UI
   *        interaction here.  May throw; `run()` calls `disconnect()` in
   *        either case.
   */
  virtual void on_session() = 0;

 private:
  // Lazily instantiated per-session proxy handles.  Created on first use;
  // cleared by run() after on_session() returns or throws so each new
  // connection starts fresh.  proxy_mutex_ guards all three optionals so
  // concurrent helper calls from on_session() are race-free.
  std::mutex proxy_mutex_;
  std::optional<bison::rmi::proxy::dynamic> import_proxy_;
  std::optional<bison::rmi::proxy::dynamic> template_proxy_;
  std::optional<bison::rmi::proxy::dynamic> fs_proxy_;

  bison::rmi::proxy::dynamic& import_proxy();
  bison::rmi::proxy::dynamic& template_proxy();
  bison::rmi::proxy::dynamic& fs_proxy();
};

}  // namespace bdg::wish
