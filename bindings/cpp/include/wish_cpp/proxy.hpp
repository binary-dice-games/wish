// MIT License © 2025 Binary Dice Games
/**
 * @file proxy.hpp
 * @brief RAII wrapper around `rmi_proxy_handle`.
 */
#pragma once

#include "error.hpp"
#include "future.hpp"
#include "key.hpp"
#include "value.hpp"

#include <rmi_c.h>

#include <functional>
#include <memory>
#include <vector>

namespace bdg::wish::binding {

/**
 * @brief RAII wrapper around a remote object proxy (`rmi_proxy_handle`).
 *
 * Mirrors `bison::rmi::proxy::dynamic` from the native client, built
 * directly on `rmi_proxy_*`/`rmi_future_*` instead of the linked RMI
 * library.
 */
class proxy {
 public:
  using event_handler = std::function<void(value)>;

  proxy() = default;
  explicit proxy(rmi_proxy_handle h) : h_(h) {}  // adopts ownership

  proxy(const proxy&) = delete;
  proxy& operator=(const proxy&) = delete;

  proxy(proxy&& other) noexcept : h_(other.h_), handlers_(std::move(other.handlers_)) { other.h_ = nullptr; }
  proxy& operator=(proxy&& other) noexcept {
    if (this != &other) {
      if (h_) rmi_proxy_release(h_);
      h_ = other.h_;
      handlers_ = std::move(other.handlers_);
      other.h_ = nullptr;
    }
    return *this;
  }

  ~proxy() {
    if (h_) rmi_proxy_release(h_);
  }

  bool valid() const noexcept { return h_ != nullptr; }
  rmi_proxy_handle handle() const noexcept { return h_; }

  /** @brief Clears explicitly set fields, reverting to the object's defaults. */
  void clear(int64_t timeout_ms = -1) {
    detail::throw_if_rmi_error(rmi_proxy_clear(h_, timeout_ms), "proxy::clear");
  }

  future clear_async() {
    rmi_future_handle f = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_clear_async(h_, &f), "proxy::clear_async");
    return future(f);
  }

  /** @brief Applies a partial field update; one-way, no round-trip wait beyond `timeout_ms`. */
  void set(const value& fields, int64_t timeout_ms = -1) {
    detail::throw_if_rmi_error(rmi_proxy_set(h_, fields.handle(), timeout_ms), "proxy::set");
  }

  future set_async(const value& fields) {
    rmi_future_handle f = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_set_async(h_, fields.handle(), &f), "proxy::set_async");
    return future(f);
  }

  /** @brief Retrieves a full field snapshot. */
  value get(int64_t timeout_ms = -1) const {
    bison_handle out = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_get(h_, nullptr, &out, timeout_ms), "proxy::get");
    return value::adopt(out);
  }

  /** @brief Retrieves only the fields named in @p projection. */
  value get(const value& projection, int64_t timeout_ms = -1) const {
    bison_handle out = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_get(h_, projection.handle(), &out, timeout_ms), "proxy::get");
    return value::adopt(out);
  }

  future get_async() const {
    rmi_future_handle f = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_get_async(h_, nullptr, &f), "proxy::get_async");
    return future(f);
  }

  future get_async(const value& projection) const {
    rmi_future_handle f = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_get_async(h_, projection.handle(), &f), "proxy::get_async");
    return future(f);
  }

  value call(key_t method, const value& params = value{}, int64_t timeout_ms = -1) {
    bison_handle out = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_call(h_, method, params.handle(), &out, timeout_ms), "proxy::call");
    return value::adopt(out);
  }

  future call_async(key_t method, const value& params = value{}) {
    rmi_future_handle f = nullptr;
    detail::throw_if_rmi_error(rmi_proxy_call_async(h_, method, params.handle(), &f), "proxy::call_async");
    return future(f);
  }

  /**
   * @brief Subscribes to a server-initiated event.
   *
   * @p handler is kept alive for the lifetime of this proxy (owned in
   * `handlers_`); the underlying C ABI only holds a raw callback pointer.
   */
  void on_event(key_t name, event_handler handler) {
    auto holder = std::make_unique<event_handler>(std::move(handler));
    void* user = holder.get();
    handlers_.push_back(std::move(holder));
    detail::throw_if_rmi_error(rmi_proxy_on_event(h_, name, &proxy::event_trampoline, user), "proxy::on_event");
  }

 private:
  static void event_trampoline(bison_handle params, void* user) {
    // `params` is only valid for the duration of this call and must not be
    // released by us -- take our own reference before handing it to `value`
    // (whose destructor releases it).
    auto* handler = static_cast<event_handler*>(user);
    (*handler)(value::adopt(params ? bison_add_ref(params) : nullptr));
  }

  rmi_proxy_handle h_ = nullptr;
  std::vector<std::unique_ptr<event_handler>> handlers_;
};

inline proxy future::get_proxy() {
  rmi_proxy_handle out = nullptr;
  rmi_error rc = rmi_future_get_proxy(&h_, &out);
  detail::throw_if_rmi_error(rc, "future::get_proxy");
  return proxy(out);
}

}  // namespace bdg::wish::binding
