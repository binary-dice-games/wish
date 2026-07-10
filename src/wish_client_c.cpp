// MIT License © 2025 Binary Dice Games
/// @file wish_client_c.cpp
/// @brief C ABI implementation for the wish client shared library.
///
/// Wraps wish::client in a plain-C interface so that any language with a
/// C FFI can drive a wish session without linking against C++ directly.
#include <client/client.hpp>
#include <ui/ui_descriptor.hpp>
#include <include/wish_client_c.h>

#include "src/rmi/transport/named_pipe_transport.hpp"
#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/stream_transport.hpp"
#include "src/rmi/transport/term_transport.hpp"
#include "src/term/scoped_terminal_config.hpp"

// bison_c.cpp's and rmi_c.cpp's handle-wrapping helpers (sp_dyn/as_handle,
// proxy_ptr/as_proxy_handle, bool_future_state, proxy_future_state,
// store_future_handle, ...) are file-local statics — bison_handle,
// rmi_proxy_handle, and rmi_future_handle are otherwise fully opaque outside
// those translation units. #including the sources directly gives this file
// access to them so functions here can return/build real bison_handle /
// rmi_proxy_handle / rmi_future_handle values instead of wish-private handle
// types. Their private helper names don't collide with each other. See the
// CMakeLists.txt comment above the wish_client_dll target: neither file may
// also be compiled as a separate source of this target.
#include "src/bison/bison_c.cpp"
#include "src/rmi/rmi_c.cpp"

#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// Convenience aliases — mirror the naming convention used by calculator.
using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Forward declaration (circular dependency: c_abi_client ↔ wish_client_handle_) ──

struct wish_client_handle_;

// ── Internal C++ client ───────────────────────────────────────────────────────

/// Subclass of wish::client that calls the C session callback from on_session.
class c_abi_client : public wish::client {
 public:
  c_abi_client(std::unique_ptr<rmi::transport::client_transport_iface> t, wish_client_handle_* state)
      : wish::client(std::move(t)), state_(state) {}

 protected:
  void on_session() override;

 private:
  wish_client_handle_* state_;
};

// ── Client state ──────────────────────────────────────────────────────────────

struct wish_client_handle_ {
  // Only engaged by wish_client_term_create(). Declared before client_ so
  // that member destruction order (reverse of declaration) tears client_
  // down first: its owned term_client_transport calls back into this via
  // its before_destroy hook (scoped_terminal_config::stop_output_pump()),
  // which must still be alive when that happens, and only afterwards does
  // scoped_terminal_config's own destructor restore the terminal.
  std::optional<term::scoped_terminal_config> term_config_;

  std::unique_ptr<c_abi_client> client_;

  wish_session_fn session_fn_ = nullptr;
  void* session_ud_ = nullptr;

  std::mutex wait_mtx_;
  std::condition_variable wait_cv_;
  bool quit_ = false;

  // Cache of the most recent instantiate_template() result: dot-path ->
  // proxy. wish_proxy_get() builds a fresh, independently-owned
  // rmi_proxy_handle from the cached remote id via client_->make_proxy() —
  // a purely local operation, no server round trip — on every call.
  wish::proxy_map proxy_map_;

  // Owned stream for wish_client_stream_create(); kept alive as long as transport.
  std::fstream stream_storage_;

  std::string last_error_;
};

void c_abi_client::on_session() {
  if (state_->session_fn_)
    state_->session_fn_(state_, state_->session_ud_);
}

// ── wish_key ──────────────────────────────────────────────────────────────────

extern "C" wish_hash wish_key(const char* name) {
  return bison_key(name);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

// Wraps a freshly constructed transport in a c_abi_client and stores it in
// the handle; shared tail of every wish_client_*_create() below.
static wish_client_handle
make_client_handle(std::unique_ptr<wish_client_handle_> state, std::unique_ptr<rmi::transport::client_transport_iface> t) {
  state->client_ = std::make_unique<c_abi_client>(std::move(t), state.get());
  return state.release();
}

extern "C" wish_client_handle wish_client_tcp_create(const char* host, uint16_t port) {
  if (!host)
    return nullptr;
  try {
    auto state = std::make_unique<wish_client_handle_>();
    return make_client_handle(std::move(state), std::make_unique<socket_client_transport>(host, port));
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_client_handle wish_client_stream_create(const char* path) {
  if (!path)
    return nullptr;
  try {
    auto state = std::make_unique<wish_client_handle_>();
    state->stream_storage_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!state->stream_storage_.is_open()) {
      state->last_error_ = std::string("cannot open stream path: ") + path;
      return nullptr;
    }
    auto t = std::make_unique<stream_client_transport>(state->stream_storage_);
    return make_client_handle(std::move(state), std::move(t));
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_client_handle wish_client_pipe_create(const char* path) {
  if (!path)
    return nullptr;
  try {
    auto state = std::make_unique<wish_client_handle_>();
    return make_client_handle(std::move(state), std::make_unique<named_pipe_client_transport>(path));
  } catch (...) {
    return nullptr;
  }
}

extern "C" wish_client_handle wish_client_term_create(void) {
  try {
    auto state = std::make_unique<wish_client_handle_>();
    // Mirrors bison::app::client_app::run()'s term-transport branch: puts fd
    // 0/1 into raw mode and redirects them through internal pipes so the
    // transport becomes the sole reader/writer of the real fds, while
    // scoped_terminal_config re-synthesizes plain, protocol-unaware terminal
    // I/O (line discipline, local echo) on top for anything that isn't part
    // of a framed envelope. Without this, fd 0 stays in cooked mode and the
    // peer's raw `BISON<...>` marker bytes are never intercepted by the
    // transport — they show up as literal, echoed terminal input instead.
    state->term_config_.emplace(term::scoped_terminal_config::params{0, 1});
    auto& stc = *state->term_config_;
    auto t = std::make_unique<term_client_transport>(
        stc.upstream_read_fd(),
        stc.upstream_write_fd(),
        [&stc](std::string_view s) { stc.on_passthrough(s); },
        kDefaultHandshakeTimeout,
        [&stc] { stc.stop_output_pump(); });
    stc.set_output_channel([raw = t.get()](std::string_view s) { raw->send(s); });
    return make_client_handle(std::move(state), std::move(t));
  } catch (...) {
    return nullptr;
  }
}

extern "C" void wish_client_destroy(wish_client_handle c) {
  delete c;
}

extern "C" wish_error wish_client_run(wish_client_handle c, wish_session_fn fn, void* ud) {
  if (!c)
    return WISH_ERR_NULL;
  c->session_fn_ = fn;
  c->session_ud_ = ud;
  c->quit_ = false;
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

extern "C" void wish_client_wait(wish_client_handle c) {
  if (!c)
    return;
  std::unique_lock<std::mutex> lk(c->wait_mtx_);
  c->wait_cv_.wait(lk, [c] { return c->quit_; });
}

extern "C" void wish_client_quit(wish_client_handle c) {
  if (!c)
    return;
  {
    std::lock_guard<std::mutex> lk(c->wait_mtx_);
    c->quit_ = true;
  }
  c->wait_cv_.notify_all();
}

extern "C" const char* wish_last_error(wish_client_handle c) {
  if (!c)
    return "";
  return c->last_error_.c_str();
}

// ── Style ─────────────────────────────────────────────────────────────────────

extern "C" wish_error wish_set_style_preset(wish_client_handle c, const char* preset) {
  if (!c || !preset)
    return WISH_ERR_NULL;
  try {
    c->client_->set_style_preset(std::string{preset}).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error
wish_set_style_preset_async(wish_client_handle c, const char* preset, rmi_future_handle* out_future) {
  if (!c || !preset || !out_future)
    return WISH_ERR_NULL;
  try {
    std::string preset_name{preset};
    std::future<bool> fut = std::async(std::launch::async, [c, preset_name]() -> bool {
      c->client_->set_style_preset(preset_name).get();
      return true;
    });
    return store_future_handle<bool_future_state>(out_future, std::move(fut)) == RMI_OK ? WISH_OK : WISH_ERR_EXCEPTION;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

// ── Templates ─────────────────────────────────────────────────────────────────

extern "C" wish_error wish_register_template(wish_client_handle c, const char* name, const char* descriptor) {
  if (!c || !name || !descriptor)
    return WISH_ERR_NULL;
  try {
    c->client_->register_template(bdg::bison::key_t{name}, bdg::wish::import_descriptor_text(descriptor)).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error wish_register_template_async(
    wish_client_handle c, const char* name, const char* descriptor, rmi_future_handle* out_future) {
  if (!c || !name || !descriptor || !out_future)
    return WISH_ERR_NULL;
  try {
    bdg::bison::key_t key{name};
    bdg::bison::dynamic desc = bdg::wish::import_descriptor_text(descriptor);
    std::future<bool> fut = std::async(std::launch::async, [c, key, d = std::move(desc)]() mutable -> bool {
      c->client_->register_template(key, std::move(d)).get();
      return true;
    });
    return store_future_handle<bool_future_state>(out_future, std::move(fut)) == RMI_OK ? WISH_OK : WISH_ERR_EXCEPTION;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

// Merge a freshly instantiated template's proxies into the client's
// persistent proxy_map_ under `prefix` (caller-supplied, so distinct
// instances of the same template can coexist), the same prefixing
// convention as ui_tree::merge(): the root (key "") lands at just `prefix`,
// descendants land at `prefix + "." + path`. Unlike a plain assignment, this
// leaves other instances' entries in place so wish_proxy_get() can still
// resolve them; instantiating again under the same prefix only replaces
// that instance's own subtree.
static void merge_proxy_map(wish_client_handle c, const std::string& prefix, wish::proxy_map&& fresh) {
  for (auto& [path, proxy] : fresh)
    c->proxy_map_.insert_or_assign(path.empty() ? prefix : (prefix + "." + path), std::move(proxy));
}

// Inverse of merge_proxy_map(): erase `prefix` and every "prefix." descendant
// from proxy_map_. Returns the number of entries erased.
static size_t erase_proxy_subtree(wish_client_handle c, const std::string& prefix) {
  std::string child_prefix = prefix + ".";
  size_t erased = 0;
  for (auto it = c->proxy_map_.begin(); it != c->proxy_map_.end();) {
    if (it->first == prefix || it->first.compare(0, child_prefix.size(), child_prefix) == 0) {
      it = c->proxy_map_.erase(it);
      ++erased;
    } else {
      ++it;
    }
  }
  return erased;
}

extern "C" rmi_proxy_handle wish_instantiate_template(wish_client_handle c, const char* name, const char* prefix) {
  if (!c || !name || !prefix)
    return nullptr;
  try {
    std::string prefix_str{prefix};
    merge_proxy_map(c, prefix_str, c->client_->instantiate_template(bdg::bison::key_t{name}).get());

    auto it = c->proxy_map_.find(prefix_str);
    if (it == c->proxy_map_.end())
      return nullptr;

    auto* pp = new proxy_ptr(std::make_unique<proxy::dynamic>(c->client_->make_proxy(it->second.object_id())));
    return as_proxy_handle(pp);
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return nullptr;
  }
}

extern "C" wish_error wish_instantiate_template_async(
    wish_client_handle c, const char* name, const char* prefix, rmi_future_handle* out_future) {
  if (!c || !name || !prefix || !out_future)
    return WISH_ERR_NULL;
  try {
    bdg::bison::key_t key{name};
    std::string prefix_str{prefix};
    std::future<proxy::dynamic> fut = std::async(std::launch::async, [c, key, prefix_str]() -> proxy::dynamic {
      merge_proxy_map(c, prefix_str, c->client_->instantiate_template(key).get());
      auto it = c->proxy_map_.find(prefix_str);
      if (it == c->proxy_map_.end())
        throw std::runtime_error("template has no root element");
      return c->client_->make_proxy(it->second.object_id());
    });
    return store_future_handle<proxy_future_state>(out_future, std::move(fut)) == RMI_OK ? WISH_OK
                                                                                           : WISH_ERR_EXCEPTION;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" rmi_proxy_handle wish_proxy_get(wish_client_handle c, const char* dot_path) {
  if (!c || !dot_path)
    return nullptr;
  auto it = c->proxy_map_.find(std::string{dot_path});
  if (it == c->proxy_map_.end()) {
    c->last_error_ = std::string{"proxy not found: "} + dot_path;
    return nullptr;
  }
  auto* pp = new proxy_ptr(std::make_unique<proxy::dynamic>(c->client_->make_proxy(it->second.object_id())));
  return as_proxy_handle(pp);
}

extern "C" wish_error wish_release(wish_client_handle c, const char* prefix) {
  if (!c || !prefix)
    return WISH_ERR_NULL;
  if (erase_proxy_subtree(c, std::string{prefix}) == 0) {
    c->last_error_ = std::string{"no proxies found with prefix: "} + prefix;
    return WISH_ERR_NOT_FOUND;
  }
  return WISH_OK;
}

// ── Object instantiation ────────────────────────────────────────────────────────

extern "C" rmi_proxy_handle
wish_instantiate(wish_client_handle c, wish_hash ns, wish_hash klass, bison_handle params) {
  if (!c)
    return nullptr;
  try {
    dynamic dyn_params = bison_handle_to_dynamic(params);
    proxy::dynamic proxy_obj =
        c->client_->instantiate(bdg::bison::key_t{ns}, bdg::bison::key_t{klass}, std::move(dyn_params)).get();
    auto* pp = new proxy_ptr(std::make_unique<proxy::dynamic>(std::move(proxy_obj)));
    return as_proxy_handle(pp);
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return nullptr;
  }
}

// ── File transfer ────────────────────────────────────────────────────────────────

extern "C" wish_error
wish_upload_file(wish_client_handle c, const char* name, const char* data, size_t data_len) {
  if (!c || !name || (!data && data_len > 0))
    return WISH_ERR_NULL;
  try {
    c->client_->upload_file(std::string{name}, std::string{data, data_len}).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error
wish_download_file(wish_client_handle c, const char* name, char** out_data, size_t* out_len) {
  if (!c || !name || !out_data || !out_len)
    return WISH_ERR_NULL;
  try {
    std::string data = c->client_->download_file(std::string{name}).get();
    char* buf = new char[data.size()];
    std::memcpy(buf, data.data(), data.size());
    *out_data = buf;
    *out_len = data.size();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

// ── Logging ───────────────────────────────────────────────────────────────────

extern "C" wish_error wish_log(wish_client_handle c, const char* level, const char* msg) {
  if (!c || !level || !msg)
    return WISH_ERR_NULL;
  try {
    c->client_->log(std::string{level}, std::string{msg}).get();
    return WISH_OK;
  } catch (const std::exception& e) {
    c->last_error_ = e.what();
    return WISH_ERR_EXCEPTION;
  }
}

extern "C" wish_error wish_log_debug(wish_client_handle c, const char* msg) {
  return wish_log(c, "debug", msg);
}
extern "C" wish_error wish_log_info(wish_client_handle c, const char* msg) {
  return wish_log(c, "info", msg);
}
extern "C" wish_error wish_log_warn(wish_client_handle c, const char* msg) {
  return wish_log(c, "warn", msg);
}
extern "C" wish_error wish_log_error(wish_client_handle c, const char* msg) {
  return wish_log(c, "error", msg);
}
