// MIT License © 2025 Binary Dice Games
/// @file server.cpp
/// @brief Implementation of wish::server.
#include <wish/server.hpp>
#include <wish/file_service.hpp>
#include <wish/registry.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace bdg::wish {

using namespace bison::rmi::transport;

// ── server ────────────────────────────────────────────────────────────────────

server::server(
    server_transport_iface& transport,
    std::unique_ptr<renderer> r)
    : bison::rmi::server(transport), renderer_(std::move(r)) {}

server::~server() {
  if (running_.load(std::memory_order_acquire)) {
    try {
      stop();
    } catch (...) {}
  }
}

void server::start() {
  register_all();
  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this]() { render_loop(); });
  listen();
}

void server::stop() {
  running_.store(false, std::memory_order_release);
  bison::rmi::server::stop();
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
}

void server::on_session_created(bison::rmi::context& ctx) {
  auto sess = std::make_shared<session>(ctx.session_id);
  register_file_service(*sess);
  sessions_.wlock()->emplace(ctx.session_id.id, sess);
  on_session_created(*sess);
}

void server::on_session_destroyed(bison::rmi::context& ctx) {
  std::shared_ptr<session> sess;
  {
    auto lp = sessions_.wlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end()) {
      sess = it->second;
      lp->erase(it);
    }
  }
  if (sess) {
    try {
      on_session_destroyed(*sess);
    } catch (...) {}
  }
}

void server::render_loop() {
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      renderer_->begin_frame();
      {
        auto lp = sessions_.rlock();
        for (const auto& [id, sess] : *lp) {
          auto it = sess->objects.find("");
          if (it != sess->objects.end()) {
            renderer_->render_node(*it->second, *sess);
          }
        }
      }
      renderer_->end_frame();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
}

} // namespace bdg::wish
