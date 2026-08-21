// MIT License © 2025 Binary Dice Games
/**
 * @file client.hpp
 * @brief RAII wrapper around `wish_client_handle` -- the entry point of the
 *        header-only C++ binding.
 *
 * Mirrors the ergonomics of the native, linked `bdg::wish::client`
 * (src/client/client.hpp) as closely as the C ABI allows, built entirely on
 * `wish_client_c.h` (plus `bison_c.h`/`rmi_c.h`) and the prebuilt
 * `wish_client_dll` shared library -- no bison/wish source needs to be
 * compiled into the consuming application.
 *
 * One difference from the native client: `instantiate_template()` there
 * returns a `proxy_map` (dot-path -> proxy) built up entirely on the client
 * side. The C ABI already tracks that same dot-path -> proxy map internally
 * per `wish_client_handle` (populated by `wish_instantiate_template()`,
 * queried by `wish_proxy_get()`, released by `wish_release()`), so this
 * binding uses that point-lookup ergonomic directly instead of
 * reimplementing the map client-side.
 */
#pragma once

#include "error.hpp"
#include "future.hpp"
#include "key.hpp"
#include "proxy.hpp"
#include "value.hpp"

#include <wish_client_c.h>

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish::binding {

/**
 * @brief List every embedded app registered by an enabled optional module
 *        (see modules/README.md), as a raw JSON array string --
 *        `[{"name","organization","collection","description","params":[...]}, ...]`.
 *
 * Mirrors `wish client --list`. Does not require a connection -- app
 * registration happens at library-load time. Returned as JSON text (rather
 * than parsed into `value`s) because `bison_c.h` has no generic
 * object-array accessor; parse it with `value::parse_json()` plus your own
 * traversal, or any JSON library, if you need structured access.
 */
inline std::string list_apps_json() {
  char* out = nullptr;
  detail::throw_if_wish_error(wish_list_apps(&out), "list_apps_json");
  std::string result = out ? out : "";
  bison_free_string(out);
  return result;
}

/**
 * @brief RAII wrapper around a `wish_client_handle`.
 *
 * Construct via `tcp()`, `tls()`, `stream()`, `pipe()`, or `term()`, then call
 * `run()` to connect, drive the session, and disconnect -- mirroring
 * `wish_client_run_with_params()`: the session callback runs on the
 * library's RMI worker thread and `run()` blocks until it returns (or a
 * concurrent `quit()` unblocks a `wait()` inside it).
 */
class client {
 public:
  using session_fn = std::function<void(client&)>;

  client(const client&) = delete;
  client& operator=(const client&) = delete;

  client(client&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
  client& operator=(client&& other) noexcept {
    if (this != &other) {
      destroy();
      h_ = other.h_;
      other.h_ = nullptr;
    }
    return *this;
  }

  ~client() { destroy(); }

  static client tcp(const std::string& host, uint16_t port) {
    wish_client_handle h = wish_client_tcp_create(host.c_str(), port);
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::tcp: wish_client_tcp_create failed");
    return client(h);
  }

  /**
   * @brief Create a TLS-secured TCP socket client (not yet connected).
   *
   * TLS trust/identity material (`ca_file`/`ca_pem`, `insecure_skip_verify`,
   * `cert_file`/`cert_pem`, `key_file`/`key_pem`, `key_password`,
   * `server_name`) is supplied via `run()`'s `connect_params`.
   */
  static client tls(const std::string& host, uint16_t port) {
    wish_client_handle h = wish_client_tls_create(host.c_str(), port);
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::tls: wish_client_tls_create failed");
    return client(h);
  }

  static client stream(const std::string& path) {
    wish_client_handle h = wish_client_stream_create(path.c_str());
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::stream: wish_client_stream_create failed");
    return client(h);
  }

  static client pipe(const std::string& path) {
    wish_client_handle h = wish_client_pipe_create(path.c_str());
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::pipe: wish_client_pipe_create failed");
    return client(h);
  }

  static client term() {
    wish_client_handle h = wish_client_term_create();
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::term: wish_client_term_create failed");
    return client(h);
  }

  /** @brief Last error message recorded for this client (empty if none). */
  std::string last_error() const {
    const char* msg = wish_last_error(h_);
    return msg ? msg : "";
  }

  // ── Session lifecycle ────────────────────────────────────────────────

  /**
   * @brief Connect, invoke `session_fn(*this)`, then disconnect.
   *
   * Blocks until `session_fn` returns. It runs on the RMI worker thread;
   * call `wait()` inside it to keep the session alive while event handlers
   * update the UI, and end it with `quit()` (typically from an event
   * handler). @p connect_params is forwarded to both the transport's
   * connection setup and the server's connect handshake payload.
   */
  void run(session_fn fn, const value& connect_params = value{}) {
    session_cb_ = std::move(fn);
    session_exception_ = nullptr;
    wish_error rc = wish_client_run_with_params(h_, &client::session_trampoline, this, connect_params.handle());
    session_cb_ = nullptr;
    if (session_exception_) std::rethrow_exception(session_exception_);
    detail::throw_if_wish_error(rc, "client::run", h_);
  }

  /** @brief Blocks until `quit()` is called (from any thread). */
  void wait() { wish_client_wait(h_); }

  /** @brief Signals the session to end; unblocks a concurrent `wait()`. */
  void quit() { wish_client_quit(h_); }

  // ── Style ────────────────────────────────────────────────────────────

  /** @brief Applies a built-in style preset: "dark", "light", or "classic". */
  void set_style_preset(const std::string& preset) {
    detail::throw_if_wish_error(wish_set_style_preset(h_, preset.c_str()), "client::set_style_preset", h_);
  }

  future set_style_preset_async(const std::string& preset) {
    rmi_future_handle f = nullptr;
    detail::throw_if_wish_error(
        wish_set_style_preset_async(h_, preset.c_str(), &f), "client::set_style_preset_async", h_);
    return future(f);
  }

  // ── Template management ──────────────────────────────────────────────

  /** @brief Registers a named UI template (JSON or YAML descriptor text). */
  void register_template(const std::string& name, const std::string& descriptor) {
    detail::throw_if_wish_error(
        wish_register_template(h_, name.c_str(), descriptor.c_str()), "client::register_template(" + name + ")", h_);
  }

  future register_template_async(const std::string& name, const std::string& descriptor) {
    rmi_future_handle f = nullptr;
    detail::throw_if_wish_error(
        wish_register_template_async(h_, name.c_str(), descriptor.c_str(), &f),
        "client::register_template_async(" + name + ")", h_);
    return future(f);
  }

  /**
   * @brief Instantiates a registered template under dot-path @p prefix and
   *        returns a proxy to its root; descendants are reachable via
   *        `proxy_get("prefix.child.path")`.
   */
  proxy instantiate_template(const std::string& name, const std::string& prefix) {
    rmi_proxy_handle h = wish_instantiate_template(h_, name.c_str(), prefix.c_str());
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::instantiate_template(" + name + ", " + prefix + "): " + last_error());
    return proxy(h);
  }

  future instantiate_template_async(const std::string& name, const std::string& prefix) {
    rmi_future_handle f = nullptr;
    detail::throw_if_wish_error(
        wish_instantiate_template_async(h_, name.c_str(), prefix.c_str(), &f),
        "client::instantiate_template_async(" + name + ", " + prefix + ")", h_);
    return future(f);
  }

  /** @brief Resolves a dot-joined element path (see `instantiate_template()`) from the client's local proxy map. */
  proxy proxy_get(const std::string& dot_path) {
    rmi_proxy_handle h = wish_proxy_get(h_, dot_path.c_str());
    if (!h) throw error(WISH_ERR_NOT_FOUND, "client::proxy_get(" + dot_path + ")");
    return proxy(h);
  }

  /** @brief Releases every proxy cached under @p prefix and its descendants. */
  void release(const std::string& prefix) {
    detail::throw_if_wish_error(wish_release(h_, prefix.c_str()), "client::release(" + prefix + ")", h_);
  }

  // ── Object instantiation ─────────────────────────────────────────────

  /**
   * @brief Instantiates a remote object directly (no UI template involved).
   *
   * Unlike `instantiate_template()`, the result is not merged into the
   * dot-path proxy map used by `proxy_get()`; the caller keeps and releases
   * the returned proxy directly.
   */
  proxy instantiate(key_t klass, key_t ns = key_t{0u}, const value& params = value{}) {
    rmi_proxy_handle h = wish_instantiate(h_, ns, klass, params.handle());
    if (!h) throw error(WISH_ERR_EXCEPTION, "client::instantiate: " + last_error());
    return proxy(h);
  }

  // ── Embedded apps ────────────────────────────────────────────────────

  /**
   * @brief Connects, runs the named embedded app (see `list_apps_json()`),
   *        blocks until it signals completion, then disconnects.
   *
   * Mirrors `wish client --run=<name> -- <args...>`. @p name may be a short
   * name (e.g. "bc") or its fully-qualified
   * "organization/collection/name" form.
   */
  void run_app(const std::string& name, const std::vector<std::string>& args = {}) {
    std::vector<const char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.c_str());
    detail::throw_if_wish_error(
        wish_run_app(h_, name.c_str(), argv.empty() ? nullptr : argv.data(), argv.size()),
        "client::run_app(" + name + ")", h_);
  }

  // ── File transfer ────────────────────────────────────────────────────

  /** @brief Uploads a file to the server's sandboxed session resource directory. */
  void upload_file(const std::string& name, const std::string& data) {
    detail::throw_if_wish_error(
        wish_upload_file(h_, name.c_str(), data.data(), data.size()), "client::upload_file(" + name + ")", h_);
  }

  /** @brief Downloads a previously uploaded file from the server. */
  std::string download_file(const std::string& name) {
    char* out = nullptr;
    size_t len = 0;
    detail::throw_if_wish_error(
        wish_download_file(h_, name.c_str(), &out, &len), "client::download_file(" + name + ")", h_);
    std::string result(out, len);
    bison_free_string(out);
    return result;
  }

  /** @brief Uploads a file, streaming it in chunks from a local file on disk. */
  void upload_file_from_path(const std::string& name, const std::string& local_path) {
    detail::throw_if_wish_error(
        wish_upload_file_from_path(h_, name.c_str(), local_path.c_str()),
        "client::upload_file_from_path(" + name + ")", h_);
  }

  /** @brief Downloads a file, streaming it directly to a local file on disk. */
  void download_file_to_path(const std::string& name, const std::string& local_path) {
    detail::throw_if_wish_error(
        wish_download_file_to_path(h_, name.c_str(), local_path.c_str()),
        "client::download_file_to_path(" + name + ")", h_);
  }

  /** @brief Uploads a local zip archive and has the server unpack it into a sandboxed destination directory. */
  void upload_package(const std::string& dest_path, const std::string& local_zip_path) {
    detail::throw_if_wish_error(
        wish_upload_package_from_path(h_, dest_path.c_str(), local_zip_path.c_str()),
        "client::upload_package(" + dest_path + ")", h_);
  }

  // ── Logging ──────────────────────────────────────────────────────────

  void log(const std::string& level, const std::string& msg) {
    detail::throw_if_wish_error(wish_log(h_, level.c_str(), msg.c_str()), "client::log", h_);
  }
  void log_debug(const std::string& msg) {
    detail::throw_if_wish_error(wish_log_debug(h_, msg.c_str()), "client::log_debug", h_);
  }
  void log_info(const std::string& msg) {
    detail::throw_if_wish_error(wish_log_info(h_, msg.c_str()), "client::log_info", h_);
  }
  void log_warn(const std::string& msg) {
    detail::throw_if_wish_error(wish_log_warn(h_, msg.c_str()), "client::log_warn", h_);
  }
  void log_error(const std::string& msg) {
    detail::throw_if_wish_error(wish_log_error(h_, msg.c_str()), "client::log_error", h_);
  }

  wish_client_handle handle() const noexcept { return h_; }

 private:
  explicit client(wish_client_handle h) : h_(h) {}

  void destroy() {
    if (h_) wish_client_destroy(h_);
    h_ = nullptr;
  }

  static void session_trampoline(wish_client_handle, void* userdata) {
    auto* self = static_cast<client*>(userdata);
    try {
      self->session_cb_(*self);
    } catch (...) {
      self->session_exception_ = std::current_exception();
    }
  }

  wish_client_handle h_;
  session_fn session_cb_;
  std::exception_ptr session_exception_;
};

}  // namespace bdg::wish::binding
