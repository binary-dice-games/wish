// MIT License © 2025 Binary Dice Games
/// @file wish_client_c.cpp
/// @brief C ABI implementation for the wish client shared library.
///
/// Wraps wish::client in a plain-C interface so that any language with a
/// C FFI can drive a wish session without linking against C++ directly.
#include <wish/wish_client.h>
#include <wish/client.hpp>

#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/stream_transport.hpp"
#if defined(__linux__)
#  include "src/app/pty/pty_client_transport.hpp"
#endif

#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Convenience aliases — mirror the naming convention used by calculator.
using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Forward declaration (circular dependency: c_abi_client ↔ wish_client_s) ──

struct wish_client_s;

// ── Proxy handle ──────────────────────────────────────────────────────────────

struct wish_proxy_s {
  // Non-owning pointer into wish_client_s::proxy_map_.
  // unordered_map guarantees reference/pointer stability across rehash.
  rmi::proxy::dynamic* proxy;
};

// ── Internal C++ client ───────────────────────────────────────────────────────

/// Subclass of wish::client that calls the C session callback from on_session.
class c_abi_client : public wish::client {
 public:
  c_abi_client(std::unique_ptr<rmi::transport::client_transport_iface> t,
               wish_client_s* state)
      : wish::client(std::move(t)), state_(state) {}

 protected:
  void on_session() override;

 private:
  wish_client_s* state_;
};

// ── Client state ──────────────────────────────────────────────────────────────

struct wish_client_s {
  std::unique_ptr<c_abi_client> client_;

  wish_session_fn session_fn_ = nullptr;
  void*           session_ud_ = nullptr;

  std::mutex              wait_mtx_;
  std::condition_variable wait_cv_;
  bool                    quit_ = false;

  // Proxy storage — populated by wish_instantiate_template.
  // Storing by value; pointers into proxy_map_ are stable (std::unordered_map
  // reference-stability guarantee).
  wish::proxy_map                               proxy_map_;
  std::unordered_map<std::string, wish_proxy_s> handle_map_;

  // Owned stream for WISH_TRANSPORT_STREAM; kept alive as long as transport.
  std::fstream stream_storage_;

  std::string last_error_;
};

void c_abi_client::on_session() {
  if (state_->session_fn_)
    state_->session_fn_(state_, state_->session_ud_);
}

// ── wish_key ──────────────────────────────────────────────────────────────────

extern "C" wish_hash wish_key(const char* name) {
  // FNV-1a 32-bit with MSB forced to 1 — identical to bison::hash().
  uint32_t value = 0x811c9dc5u;
  while (*name) {
    value ^= static_cast<uint32_t>(static_cast<unsigned char>(*name++));
    value *= 0x01000193u;
  }
  return value | 0x80000000u;
}

// ── Transport factory ─────────────────────────────────────────────────────────

static std::unique_ptr<rmi::transport::client_transport_iface>
make_client_transport(wish_transport_t type, const char* address,
                      wish_client_s* state) {
  if (type == WISH_TRANSPORT_SOCKET) {
    std::string host = "127.0.0.1";
    uint16_t    port = 7070;
    if (address && *address) {
      std::string addr(address);
      auto colon = addr.rfind(':');
      if (colon != std::string::npos) {
        host = addr.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
      }
    }
    return std::make_unique<socket_client_transport>(host, port);
  }

  if (type == WISH_TRANSPORT_STREAM) {
    if (!address || !*address) {
      state->last_error_ = "stream transport requires a path address";
      return nullptr;
    }
    state->stream_storage_.open(
        address, std::ios::in | std::ios::out | std::ios::binary);
    if (!state->stream_storage_.is_open()) {
      state->last_error_ = std::string("cannot open stream path: ") + address;
      return nullptr;
    }
    return std::make_unique<stream_client_transport>(state->stream_storage_);
  }

  if (type == WISH_TRANSPORT_PTY) {
#if defined(__linux__)
    return std::make_unique<bdg::bison::app::pty_client_transport>();
#else
    state->last_error_ = "PTY transport is only supported on Linux";
    return nullptr;
#endif
  }

  if (type == WISH_TRANSPORT_PIPE) {
    if (!address || !*address) {
      state->last_error_ = "pipe transport requires a path address";
      return nullptr;
    }
    return std::make_unique<named_pipe_client_transport>(address);
  }

  state->last_error_ = "unknown transport type";
  return nullptr;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

extern "C" wish_client_t wish_client_create(wish_transport_t type,
                                             const char* address) {
  try {
    auto state = std::make_unique<wish_client_s>();
    auto t     = make_client_transport(type, address, state.get());
    if (!t) return nullptr;  // last_error_ already set
    state->client_ =
        std::make_unique<c_abi_client>(std::move(t), state.get());
    return state.release();
  } catch (...) {
    return nullptr;
  }
}

extern "C" void wish_client_destroy(wish_client_t c) {
  delete c;
}

extern "C" wish_error wish_client_run(wish_client_t c,
                                       wish_session_fn fn,
                                       void* ud) {
  if (!c) return WISH_ERR_NULL;
  c->session_fn_ = fn;
  c->session_ud_ = ud;
  c->quit_       = false;
  try {
    c->client_->run();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  } catch (...) {
    c->last_error_ = "unknown exception";
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" void wish_client_wait(wish_client_t c) {
  if (!c) return;
  std::unique_lock<std::mutex> lk(c->wait_mtx_);
  c->wait_cv_.wait(lk, [c] { return c->quit_; });
}

extern "C" void wish_client_quit(wish_client_t c) {
  if (!c) return;
  {
    std::lock_guard<std::mutex> lk(c->wait_mtx_);
    c->quit_ = true;
  }
  c->wait_cv_.notify_all();
}

extern "C" const char* wish_last_error(wish_client_t c) {
  if (!c) return "";
  return c->last_error_.c_str();
}

// ── Style ─────────────────────────────────────────────────────────────────────

extern "C" wish_error wish_set_style_preset(wish_client_t c,
                                             const char* preset) {
  if (!c || !preset) return WISH_ERR_NULL;
  try {
    c->client_->set_style_preset(std::string{preset}).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

// ── Templates ─────────────────────────────────────────────────────────────────

extern "C" wish_error wish_register_template(wish_client_t c,
                                              const char* name,
                                              const char* descriptor) {
  if (!c || !name || !descriptor) return WISH_ERR_NULL;
  try {
    c->client_->register_template(bdg::bison::key_t{name},
                                  std::string{descriptor}).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_proxy_t wish_instantiate_template(wish_client_t c,
                                                   const char* name) {
  if (!c || !name) return nullptr;
  try {
    c->proxy_map_.clear();
    c->handle_map_.clear();

    c->proxy_map_ = c->client_->instantiate_template(bdg::bison::key_t{name}).get();

    for (auto& [path, proxy] : c->proxy_map_)
      c->handle_map_.emplace(path, wish_proxy_s{&proxy});

    auto it = c->handle_map_.find(std::string{});
    return it != c->handle_map_.end() ? &it->second : nullptr;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return nullptr;
  }
}

extern "C" wish_proxy_t wish_proxy_get(wish_client_t c,
                                        const char* dot_path) {
  if (!c || !dot_path) return nullptr;
  auto it = c->handle_map_.find(std::string{dot_path});
  if (it == c->handle_map_.end()) {
    c->last_error_ = std::string{"proxy not found: "} + dot_path;
    return nullptr;
  }
  return &it->second;
}

// ── Proxy field setters ───────────────────────────────────────────────────────

extern "C" wish_error wish_proxy_set_string(wish_proxy_t p,
                                             wish_hash field,
                                             const char* value) {
  if (!p || !p->proxy || !value) return WISH_ERR_NULL;
  try {
    dynamic d;
    d[bdg::bison::key_t{field}] = std::string{value};
    p->proxy->set(std::move(d));
    return WISH_OK;
  } catch (...) {
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error wish_proxy_set_int(wish_proxy_t p,
                                          wish_hash field,
                                          int32_t value) {
  if (!p || !p->proxy) return WISH_ERR_NULL;
  try {
    dynamic d;
    d[bdg::bison::key_t{field}] = value;
    p->proxy->set(std::move(d));
    return WISH_OK;
  } catch (...) {
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error wish_proxy_set_float(wish_proxy_t p,
                                            wish_hash field,
                                            float value) {
  if (!p || !p->proxy) return WISH_ERR_NULL;
  try {
    dynamic d;
    d[bdg::bison::key_t{field}] = value;
    p->proxy->set(std::move(d));
    return WISH_OK;
  } catch (...) {
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error wish_proxy_set_bool(wish_proxy_t p,
                                           wish_hash field,
                                           int value) {
  if (!p || !p->proxy) return WISH_ERR_NULL;
  try {
    dynamic d;
    d[bdg::bison::key_t{field}] = (value != 0);
    p->proxy->set(std::move(d));
    return WISH_OK;
  } catch (...) {
    return WISH_ERR_EXCEPTION;
  }
}

// ── Event subscription ────────────────────────────────────────────────────────

extern "C" wish_error wish_proxy_on_event(wish_proxy_t p,
                                           const char* event,
                                           wish_event_fn callback,
                                           void* userdata) {
  if (!p || !p->proxy || !event) return WISH_ERR_NULL;
  try {
    bdg::bison::key_t key{event};
    wish_hash ev_hash = static_cast<wish_hash>(key.id);
    p->proxy->onEvent(key, [p, callback, userdata, ev_hash](dynamic) {
      if (callback) callback(p, ev_hash, userdata);
    });
    return WISH_OK;
  } catch (...) {
    return WISH_ERR_EXCEPTION;
  }
}
