// MIT License © 2025 Binary Dice Games
/// @file user_store_service.hpp
/// @brief Per-session RMI service exposing the session's own user store.
#pragma once

#include <context/persistent_store.hpp>

#include "src/bison/bison_object.hpp"

#include <memory>

namespace bdg::wish {

class user_store_service;
using user_store_service_ptr = std::shared_ptr<user_store_service>;

/**
 * @brief Gives a client access to its own persistent user store.
 *
 * Registered in the `"wish"` bison namespace as `"__WishUserStore"`. The
 * server attaches one instance to each session that has an authenticated
 * identity (see `server::on_authenticated()`); anonymous sessions get none,
 * and instantiating the class from such a session fails with a clear error.
 *
 * Wraps exactly one `persistent_store` -- the session's own user store --
 * so a client can never name another user's store, nor the server store,
 * which has no RMI class at all.
 *
 * ## RMI methods exposed to clients
 *
 * | Method  | Params                                   | Result                          |
 * |---------|------------------------------------------|---------------------------------|
 * | `get`   | `"name"`: string                         | `"found"`: bool, `"value"`: object (when found) |
 * | `set`   | `"name"`: string, `"value"`: object      | --                              |
 * | `erase` | `"name"`: string                         | `"erased"`: bool                |
 * | `keys`  | --                                       | indexed list of entry names     |
 *
 * Invalid entry names (see `persistent_store::is_valid_name()`) and write
 * failures are reported to the client as RMI errors.
 */
class user_store_service : public bison::dynamic {
 public:
  /// @brief Construct a service bound to @p store (must not be null).
  static user_store_service_ptr instantiate(persistent_store_ptr store);

  /// @param base   Prototype-initialised base (from `dynamic::instantiate`).
  /// @param store  The session's user store; must not be null.
  user_store_service(bison::dynamic&& base, persistent_store_ptr store);

  /// @brief The wrapped user store.
  const persistent_store_ptr& store() const noexcept {
    return store_;
  }

 private:
  persistent_store_ptr store_;
};

/// @brief Register the `__WishUserStore` class in the `"wish"` namespace.
void register_user_store_service();

} // namespace bdg::wish
