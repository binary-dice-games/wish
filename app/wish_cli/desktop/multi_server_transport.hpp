// MIT License © 2026 Binary Dice Games
/**
 * @file multi_server_transport.hpp
 * @brief A server transport that accepts clients from several transports at
 *        once, so `wish desktop` can listen on its configured downstream
 *        transport and on its fixed local endpoint with one bridge.
 */
#pragma once

#include "src/bison/bison.hpp"
#include "src/rmi/transport/transport_iface.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

namespace bdg::wish {

/**
 * @brief Combines one primary server transport (borrowed) with any number of
 *        extra ones (owned) behind a single `server_transport_iface`.
 *
 * `start()` starts every transport and runs one accept thread per
 * transport; `accept()` returns the next connection from any of them;
 * `stop()` stops all of them. Only the primary transport receives the
 * `start()` params; the extras are started with empty params (an extra
 * that was already started before is left as is, which is how `wish
 * desktop` hands over its endpoint after its single-instance check).
 */
class multi_server_transport : public bison::rmi::transport::server_transport_iface {
 public:
  /// @param primary Borrowed; must outlive this object.
  explicit multi_server_transport(bison::rmi::transport::server_transport_iface& primary);
  ~multi_server_transport() override;

  multi_server_transport(const multi_server_transport&) = delete;
  multi_server_transport& operator=(const multi_server_transport&) = delete;

  /// @brief Adds an extra transport. Call before start().
  void add(std::unique_ptr<bison::rmi::transport::server_transport_iface> transport);

  /**
   * @brief Starts every transport and its accept thread.
   * @throws Whatever a transport's own start() throws; transports already
   *         started by this call are stopped again first.
   */
  void start(bison::dynamic params) override;

  /// @brief Next connection from any transport; `nullptr` on timeout or
  ///        after stop().
  std::unique_ptr<bison::rmi::transport::server_connection_iface> accept(std::chrono::milliseconds timeout) override;

  /// @brief Stops every transport and joins the accept threads. Idempotent.
  void stop() override;

 private:
  using connection_ptr = std::unique_ptr<bison::rmi::transport::server_connection_iface>;

  void pump(bison::rmi::transport::server_transport_iface& transport);

  bison::rmi::transport::server_transport_iface& primary_;
  std::vector<std::unique_ptr<bison::rmi::transport::server_transport_iface>> extras_;
  std::vector<std::thread> pumps_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  bison::synchronized<std::deque<connection_ptr>> accepted_;
};

} // namespace bdg::wish
