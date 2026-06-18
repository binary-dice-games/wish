// MIT License © 2025 Binary Dice Games
/**
 * @file server.hpp
 * @brief wish::server — RMI server with wish session lifecycle management.
 */
#pragma once

#include "src/rmi/server/server.hpp"
#include <wish/renderer.hpp>
#include <wish/session.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

namespace bdg::wish {

/**
 * @brief Hosts wish UI elements over an RMI transport.
 *
 * Inherits all RMI server behaviour from `bison::rmi::server` and adds:
 * - Automatic wish class registration on `start()`.
 * - Per-client `wish::session` lifecycle (created/destroyed with the
 *   connection).
 * - A renderer-driven frame loop that calls `begin_frame` / `render_node` /
 *   `end_frame` on every connected session at a fixed rate.
 *
 * ## Usage
 *
 * Subclass and override the wish-level hooks:
 * ```cpp
 * class my_server : public wish::server {
 *  protected:
 *   void on_session_created(wish::session& s) override { ... }
 *   void on_session_destroyed(wish::session& s) override { ... }
 * };
 * ```
 *
 * @note Call `stop()` before the server is destroyed so that virtual overrides
 *       of the lifecycle callbacks fire correctly on disconnect.
 */
class server : public bison::rmi::server {
 public:
  /**
   * @brief Construct a wish server that borrows an externally-owned transport.
   * @param transport Transport owned by the caller; must outlive the server.
   * @param r         Renderer used by the frame loop.
   */
  explicit server(
      bison::rmi::transport::server_transport_iface& transport,
      std::unique_ptr<renderer> r);

  ~server();

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  /** @brief Register wish classes, start the render loop, then begin accepting
   *         client connections. */
  void start();

  /** @brief Stop the accept loop, render loop, and join all threads. */
  void stop();

 protected:
  /** @brief Called on the worker thread after a client connects. */
  virtual void on_session_created(session& s) { (void)s; }

  /** @brief Called on the worker thread just before a client disconnects. */
  virtual void on_session_destroyed(session& s) { (void)s; }

 private:
  // Bridge bison's context-level hooks to wish session management.
  // Declared final so subclasses use the wish-level hooks above.
  void on_session_created(bison::rmi::context& ctx) override final;
  void on_session_destroyed(bison::rmi::context& ctx) override final;

  void render_loop();

  std::unique_ptr<renderer> renderer_;
  bison::synchronized<
      std::unordered_map<bison::hash_t, std::shared_ptr<session>>>
      sessions_;
  std::thread render_thread_;
  std::atomic<bool> running_{false};
};

} // namespace bdg::wish
