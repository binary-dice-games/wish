// MIT License © 2025 Binary Dice Games
/// @file template_handler.hpp
/// @brief Server-side __WishTemplate RMI object for named UI templates.
#pragma once

#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>

namespace bdg::wish {

/**
 * @brief Server-side bison dynamic for the `__WishTemplate` RMI class.
 *
 * Exposes two methods:
 * - `"register"_key(name, descriptor)` — stores a named descriptor in the
 *   session's template map.
 * - `"instantiate"_key(name)` — looks up the stored descriptor, parses it,
 *   and registers the resulting objects in the session.
 *
 * `wish::server::on_create_object` calls `init()` once per instance to
 * supply the per-session context before the object is accessible via RMI.
 */
class template_handler : public bison::dynamic {
 public:
  explicit template_handler(bison::dynamic&& base);

  /**
   * @brief Inject session context.
   *
   * Called exactly once by `wish::server::on_create_object` after the object
   * is created and before it is accessible via RMI.
   *
   * @param ctx  Per-session RMI context; must outlive `*this`.
   * @param sess Shared wish session state.
   */
  void init(bison::rmi::context& ctx, std::shared_ptr<session> sess) {
    ctx_ = &ctx;
    sess_ = std::move(sess);
  }

 private:
  bison::rmi::context* ctx_{nullptr};
  std::shared_ptr<session> sess_;
};

/// @brief Register `template_handler` as the `__WishTemplate` class prototype.
void register_template_handler();

}  // namespace bdg::wish
