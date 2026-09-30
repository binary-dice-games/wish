// MIT License © 2026 Binary Dice Games
/// @file free_port_server.hpp
/// @brief Start a socket/TLS-backed `wish::server` on a free local port.
///
/// ctest runs every test case as its own process, in parallel under `-j`.
/// A hardcoded port (e.g. 17075) therefore collides whenever two test
/// processes that use it run at the same moment -- the second one's
/// `server::start()` throws "address already in use". Instead, pick ports
/// from a per-process random base and retry past bind conflicts (the same
/// approach as bison's own `make_paired_tcp_server()` in
/// `extern/bison/tests/rmi_c_tests.cpp`).
#pragma once

#include <server/renderer.hpp>
#include <server/server.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

/// A started server, its transport (which must outlive it) and its port.
template <typename Transport>
struct free_port_server {
  std::unique_ptr<Transport> transport;
  std::unique_ptr<bdg::wish::server> server;
  uint16_t port{0};
};

/**
 * @brief Construct `Transport("127.0.0.1", port)` + a `wish::server` with a
 *        `null_renderer`, and `start(start_args...)` it on the first port
 *        that binds.
 *
 * @param first_port Port to try first; 0 (default) picks a random port in
 *                   [20000, 60000), different per process, so parallel test
 *                   processes rarely even need to retry.
 * @throws std::runtime_error if 64 consecutive ports all fail to bind.
 */
template <typename Transport, typename... StartArgs>
free_port_server<Transport> start_on_free_port(uint16_t first_port, const StartArgs&... start_args) {
  static std::atomic<uint16_t> next_port{[] {
    std::random_device rd;
    return static_cast<uint16_t>(20000 + rd() % 40000);
  }()};
  uint16_t port = first_port != 0 ? first_port : next_port.fetch_add(64);
  for (int attempt = 0; attempt < 64; ++attempt, ++port) {
    free_port_server<Transport> s;
    s.transport = std::make_unique<Transport>("127.0.0.1", port);
    s.server = std::make_unique<bdg::wish::server>(*s.transport, std::make_unique<bdg::wish::null_renderer>());
    try {
      s.server->start(start_args...);
    } catch (const std::runtime_error&) {
      continue; // most likely "address already in use" -- try the next port
    }
    s.port = port;
    return s;
  }
  throw std::runtime_error("start_on_free_port: no free port found");
}
