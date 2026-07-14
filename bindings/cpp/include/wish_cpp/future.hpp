// MIT License © 2025 Binary Dice Games
/**
 * @file future.hpp
 * @brief RAII wrapper around `rmi_future_handle`.
 */
#pragma once

#include "error.hpp"
#include "value.hpp"

#include <rmi_c.h>

namespace bdg::wish::binding {

class proxy;  // defined in proxy.hpp; future::get_proxy() is defined there.

/**
 * @brief RAII wrapper around an in-flight asynchronous RMI operation.
 *
 * Resolves to either a `value` (`get()`) or a `proxy` (`get_proxy()`),
 * matching whichever `*_async` call produced it -- calling the wrong one is
 * a caller error, same as consuming a `std::future<T>` with the wrong `T`
 * would be in the native client.
 */
class future {
 public:
  future() = default;
  explicit future(rmi_future_handle h) : h_(h) {}  // adopts ownership

  future(const future&) = delete;
  future& operator=(const future&) = delete;

  future(future&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
  future& operator=(future&& other) noexcept {
    if (this != &other) {
      if (h_) rmi_future_release(h_);
      h_ = other.h_;
      other.h_ = nullptr;
    }
    return *this;
  }

  ~future() {
    if (h_) rmi_future_release(h_);
  }

  bool valid() const noexcept { return h_ != nullptr; }

  /** @brief Blocks until the operation completes; does not consume the future. */
  void wait(int64_t timeout_ms = -1) const {
    detail::throw_if_rmi_error(rmi_future_wait(h_, timeout_ms), "future::wait");
  }

  /** @brief Consumes the future and returns its `value` result. */
  value get() {
    bison_handle out = nullptr;
    rmi_error rc = rmi_future_get_dynamic(&h_, &out);
    detail::throw_if_rmi_error(rc, "future::get");
    return value::adopt(out);
  }

  /** @brief Consumes the future and returns its `proxy` result. Defined in proxy.hpp. */
  proxy get_proxy();

 private:
  rmi_future_handle h_ = nullptr;
};

}  // namespace bdg::wish::binding
