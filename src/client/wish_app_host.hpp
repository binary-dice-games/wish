// MIT License © 2025 Binary Dice Games
/**
 * @file wish_app_host.hpp
 * @brief Minimal surface an embedded app runner (bc/nano/
 *        top) needs from whatever is hosting it.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <functional>
#include <future>
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
