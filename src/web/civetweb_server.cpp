// MIT License © 2025 Binary Dice Games
/// @file civetweb_server.cpp
/// @brief Implementation of bdg::wish::civetweb_server.
#include <web/civetweb_server.hpp>

#ifdef WISH_WEB_ENABLED

#include "src/bison/bison_sync.hpp"

#include <civetweb.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace bdg::wish {

struct civetweb_server::impl {
  std::string bind_addr;
  int port = 0;
  std::filesystem::path document_root;
  on_connect_fn on_connect;
  on_disconnect_fn on_disconnect;
  on_message_fn on_message;

  mg_context* ctx = nullptr;
  bison::synchronized<std::vector<mg_connection*>> connections;

  // ── WebSocket callbacks (civetweb worker threads) ─────────────────────────
  //
  // Defined as members (not free functions) so they can name `impl` despite
  // it being a private nested type of civetweb_server.

  static int ws_connect_handler(const mg_connection* /*conn*/, void* /*cbdata*/) {
    return 0; // accept every connection; this is a localhost dev tool.
  }

  static void ws_ready_handler(mg_connection* conn, void* cbdata) {
    auto* self = static_cast<impl*>(cbdata);
    self->connections.wlock()->push_back(conn);
    if (self->on_connect)
      self->on_connect(static_cast<ws_connection_id>(conn));
  }

  static int ws_data_handler(mg_connection* conn, int bits, char* data, size_t data_len, void* cbdata) {
    auto* self = static_cast<impl*>(cbdata);
    int opcode = bits & 0x0F;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE)
      return 0;
    if (opcode == MG_WEBSOCKET_OPCODE_BINARY && self->on_message) {
      std::span<const std::byte> payload{reinterpret_cast<const std::byte*>(data), data_len};
      self->on_message(static_cast<ws_connection_id>(conn), payload);
    }
    return 1; // keep the connection open
  }

  static void ws_close_handler(const mg_connection* conn, void* cbdata) {
    auto* self = static_cast<impl*>(cbdata);
    {
      auto lock = self->connections.wlock();
      auto& conns = *lock;
      conns.erase(std::remove(conns.begin(), conns.end(), conn), conns.end());
    }
    if (self->on_disconnect)
      self->on_disconnect(static_cast<ws_connection_id>(const_cast<mg_connection*>(conn)));
  }
};

// ── construction ─────────────────────────────────────────────────────────────

civetweb_server::civetweb_server(
    std::string bind_addr,
    int port,
    std::filesystem::path document_root,
    on_connect_fn on_connect,
    on_disconnect_fn on_disconnect,
    on_message_fn on_message)
    : impl_(std::make_unique<impl>()) {
  impl_->bind_addr = std::move(bind_addr);
  impl_->port = port;
  impl_->document_root = std::move(document_root);
  impl_->on_connect = std::move(on_connect);
  impl_->on_disconnect = std::move(on_disconnect);
  impl_->on_message = std::move(on_message);
}

civetweb_server::~civetweb_server() {
  stop();
}

// ── lifecycle ─────────────────────────────────────────────────────────────────

void civetweb_server::start() {
  if (impl_->ctx)
    return;

  std::string listening_ports =
      impl_->bind_addr.empty() ? std::to_string(impl_->port) : impl_->bind_addr + ":" + std::to_string(impl_->port);
  std::string doc_root = impl_->document_root.string();

  const char* options[] = {
      "document_root",
      doc_root.c_str(),
      "listening_ports",
      listening_ports.c_str(),
      "num_threads",
      "4",
      nullptr,
  };

  mg_callbacks callbacks{};
  impl_->ctx = mg_start(&callbacks, this, options);
  if (!impl_->ctx) {
    throw std::runtime_error(
        "civetweb_server: mg_start failed (bind_addr=" + impl_->bind_addr +
        ", port=" + std::to_string(impl_->port) + ")");
  }

  mg_set_websocket_handler(impl_->ctx, "/ws", &impl::ws_connect_handler, &impl::ws_ready_handler,
                            &impl::ws_data_handler, &impl::ws_close_handler, impl_.get());
}

void civetweb_server::stop() {
  if (impl_->ctx) {
    mg_stop(impl_->ctx);
    impl_->ctx = nullptr;
    impl_->connections.wlock()->clear();
  }
}

int civetweb_server::actual_port() const {
  if (!impl_->ctx)
    return 0;
  mg_server_port ports[4];
  int n = mg_get_server_ports(impl_->ctx, 4, ports);
  if (n <= 0)
    return 0;
  return ports[0].port;
}

// ── outbound ──────────────────────────────────────────────────────────────────

void civetweb_server::broadcast(std::span<const std::byte> bytes) {
  // Snapshot the connection list under the lock, then write outside it so a
  // slow client write can't block connect/disconnect callbacks.
  std::vector<mg_connection*> targets = impl_->connections.copy();
  const char* data = reinterpret_cast<const char*>(bytes.data());
  for (mg_connection* conn : targets)
    mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_BINARY, data, bytes.size());
}

void civetweb_server::send_to(ws_connection_id id, std::span<const std::byte> bytes) {
  auto* conn = static_cast<mg_connection*>(id);
  // Unlike broadcast(), hold the lock across the write: send_to() is rare
  // (once per newly-connected client, not once per frame), so the
  // contention cost is negligible, and this closes the race where @p id
  // disconnects between the caller's decision to send and this call.
  auto lock = impl_->connections.rlock();
  const auto& conns = *lock;
  if (std::find(conns.begin(), conns.end(), conn) == conns.end())
    return; // no longer connected
  const char* data = reinterpret_cast<const char*>(bytes.data());
  mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_BINARY, data, bytes.size());
}

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
