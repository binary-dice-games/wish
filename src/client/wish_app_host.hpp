// MIT License © 2025 Binary Dice Games
/**
 * @file wish_app_host.hpp
 * @brief Minimal surface an embedded app runner (bc/nano/
 *        top) needs from whatever is hosting it.
 */
#pragma once

#include <client/app_registry.hpp>

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Progress callback for the chunked `upload_file`/`download_file`
///        overloads: invoked after each chunk with bytes transferred so far
///        and the total transfer size.
using transfer_progress_callback = std::function<void(std::uint64_t transferred, std::uint64_t total)>;

/**
 * @brief App-runner-facing interface implemented by both `wish_client_session`
 *        (transport-backed, `wish client`) and `wish_standalone_session`
 *        (in-process, `wish standalone`).
 *
 * `run_bc`/`run_nano`/`run_top` are written against
 * this interface instead of a concrete session type so the same app code
 * runs unmodified whether the session talks to a server over a transport or
 * hosts the server logic in-process.
 */
class wish_app_host {
 public:
  virtual ~wish_app_host() = default;

  /// @copydoc bdg::wish::client::instantiate
  virtual std::future<bison::rmi::proxy::dynamic>
  instantiate(bison::key_t ns, bison::key_t klass, bison::dynamic params = bison::dynamic{}) = 0;

  /// @copydoc bdg::wish::client::upload_file
  ///
  /// When @p on_progress is set, the transfer runs chunked and never blocks
  /// the calling thread for the whole transfer in one RMI round trip --
  /// @p on_progress is invoked after each chunk, so a caller running this
  /// from a background thread can post incremental UI updates without
  /// holding any lock for the full transfer duration. See
  /// `bdg::wish::standalone`'s and `bdg::wish::client`'s worker-thread/
  /// dispatch-lock contracts for why an event handler must never block on
  /// the unchunked path directly.
  ///
  /// @param on_progress Invoked after each chunk with bytes sent so far and
  ///                    the total (== `data.size()`). When left default
  ///                    (empty), the transfer is a single monolithic RMI
  ///                    call with no progress reporting.
  virtual std::future<void> upload_file(
      const std::string& name, const std::string& data, transfer_progress_callback on_progress = nullptr) = 0;

  /// @copydoc bdg::wish::client::download_file
  ///
  /// @param on_progress Invoked after each chunk with bytes received so far
  ///                    and the total file size. When left default (empty),
  ///                    the transfer is a single monolithic RMI call with no
  ///                    progress reporting.
  virtual std::future<std::string>
  download_file(const std::string& name, transfer_progress_callback on_progress = nullptr) = 0;

  // ── Private tool directory ──────────────────────────────────────────────
  //
  // Scratch files go in a temp directory inside the tool's private
  // directory, never at the sandbox root shared with other tools (see
  // file_service::app_private_dir()).

  /// @copydoc bdg::wish::client::create_temp_dir
  ///
  /// Not pure: a host without a file service (e.g. a test double) inherits
  /// this default, whose future throws `std::logic_error`.
  virtual std::future<std::string> create_temp_dir(const std::string& qualified_app) {
    (void)qualified_app;
    std::promise<std::string> p;
    p.set_exception(std::make_exception_ptr(std::logic_error("wish: temp dirs not supported by this app host")));
    return p.get_future();
  }

  /// @brief Registration info of the app this host is running, or `nullptr`
  ///        if it was not started from the app registry.
  const app_info* running_app() const {
    return running_app_;
  }

  /// @brief Records the app this host runs. Hosts call this before
  ///        `app_info::run`.
  void set_running_app(const app_info* info) {
    running_app_ = info;
  }

  /// @brief `create_temp_dir()` for the running app (see `running_app()`).
  /// @throws std::logic_error if no running app is set.
  std::future<std::string> create_app_temp_dir() {
    if (!running_app_)
      throw std::logic_error("wish: create_app_temp_dir() needs a running app");
    return create_temp_dir(qualified_app_name(*running_app_));
  }

  // ── User store ──────────────────────────────────────────────────────────
  //
  // The session's persistent, per-identity store of named bison objects
  // (see docs/persistent-store.md). Not pure: a host without one (e.g. a
  // test double) inherits these defaults, which report it as unavailable.

  /// @copydoc bdg::wish::client::has_user_store
  virtual bool has_user_store() const {
    return false;
  }

  /// @copydoc bdg::wish::client::user_store_get
  virtual std::future<std::optional<bison::dynamic>> user_store_get(const std::string& name) {
    (void)name;
    return unavailable_user_store<std::optional<bison::dynamic>>();
  }

  /// @copydoc bdg::wish::client::user_store_set
  virtual std::future<void> user_store_set(const std::string& name, bison::dynamic value) {
    (void)name;
    (void)value;
    return unavailable_user_store<void>();
  }

  /// @copydoc bdg::wish::client::user_store_erase
  virtual std::future<bool> user_store_erase(const std::string& name) {
    (void)name;
    return unavailable_user_store<bool>();
  }

  /// @copydoc bdg::wish::client::user_store_keys
  virtual std::future<std::vector<std::string>> user_store_keys() {
    return unavailable_user_store<std::vector<std::string>>();
  }

  /// @brief Store a proxy to keep the remote/local object alive for the session.
  virtual void keep_alive(bison::rmi::proxy::dynamic&& proxy) = 0;

  /// @brief Unblock the app's runner — call from a "closed" event handler.
  virtual void signal_done() = 0;

  /// @brief Set the process exit code the host reports once the app is done.
  ///
  /// For apps that are also command line tools (e.g. nymph's `render`), so a
  /// failure is visible to a calling script or agent, not only on stderr.
  /// Call before `signal_done()`. The default is 0. Hosts with no process
  /// exit code to set (the C ABI's `wish_run_app()`) ignore it.
  ///
  /// @param code  0 for success; non-zero for failure.
  virtual void set_exit_code(int /*code*/) {}

  /// @brief Positional arguments given after `--` on the command line.
  virtual const std::vector<std::string>& app_args() const = 0;

  /// @brief The UI language this session was started with (e.g. `"es"`, from
  ///        `--lang`); empty means the default (`en`).
  ///
  /// The host has already sent it to the server, so server-side forms are
  /// translated; client-side code can use it to pick its own strings.
  virtual std::string language() const {
    return {};
  }

  /// @brief Read one line of console (operator) input, blocking until a
  ///        line is available.
  ///
  /// Always safe to call regardless of transport: `wish_client_session`
  /// routes this through `bison::app::client_app::read_console_line()`,
  /// which knows how to read console input even when the active transport
  /// (e.g. `--transport=term`) also owns stdin for framed RMI traffic.
  /// App runners must use this instead of `std::cin` directly.
  ///
  /// @param line Output line, without the trailing newline.
  /// @return `false` once no more input is available (EOF).
  virtual bool read_console_line(std::string& line) = 0;

 private:
  const app_info* running_app_ = nullptr;

  template <typename T>
  static std::future<T> unavailable_user_store() {
    std::promise<T> p;
    p.set_exception(std::make_exception_ptr(std::logic_error("wish: user store not supported by this app host")));
    return p.get_future();
  }
};

} // namespace bdg::wish
