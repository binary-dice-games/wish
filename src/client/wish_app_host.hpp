// MIT License © 2025 Binary Dice Games
/**
 * @file wish_app_host.hpp
 * @brief Minimal surface an embedded app runner (calculator/notepad/
 *        process_explorer) needs from whatever is hosting it.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <future>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief App-runner-facing interface implemented by both `wish_client_session`
 *        (transport-backed, `wish client`) and `wish_standalone_session`
 *        (in-process, `wish standalone`).
 *
 * `run_calculator`/`run_notepad`/`run_process_explorer` are written against
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
  virtual std::future<void> upload_file(const std::string& name, const std::string& data) = 0;

  /// @copydoc bdg::wish::client::download_file
  virtual std::future<std::string> download_file(const std::string& name) = 0;

  /// @brief Store a proxy to keep the remote/local object alive for the session.
  virtual void keep_alive(bison::rmi::proxy::dynamic&& proxy) = 0;

  /// @brief Unblock the app's runner — call from a "closed" event handler.
  virtual void signal_done() = 0;

  /// @brief Positional arguments given after `--` on the command line.
  virtual const std::vector<std::string>& app_args() const = 0;

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
};

} // namespace bdg::wish
