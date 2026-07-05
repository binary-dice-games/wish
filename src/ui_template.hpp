// MIT License © 2025 Binary Dice Games
/// @file ui_template.hpp
/// @brief Server-side __WishTemplate RMI object for named UI templates.
#pragma once

#include <session.hpp>

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
class ui_template : public bison::dynamic {
 public:
  explicit ui_template(bison::dynamic&& base);

  /**
   * @brief Inject session context.
   *
   * Called exactly once by `wish::server::on_create_object` after the object
   * is created and before it is accessible via RMI.
   *
   * @param ctx  Per-session RMI context; must outlive `*this`.
   * @param sess Shared wish session state.
   */
  void init(bison::rmi::context& ctx, sync_session_ptr sync_sess) {
    ctx_ = &ctx;
    sync_sess_ = std::move(sync_sess);
  }

  bison::dynamic do_register(const bison::dynamic& params);
  bison::dynamic do_instantiate(const bison::dynamic& params);

  /// @throws std::logic_error if called outside RMI dispatch.
  session& sess() {
    if (!detail::current_session)
      throw std::logic_error("wish: ui_template method called outside RMI dispatch");
    return *detail::current_session;
  }

 private:
  bison::rmi::context* ctx_{nullptr};
  sync_session_ptr sync_sess_;
};

/// @brief Register `ui_template` as the `__WishTemplate` class prototype.
void register_ui_template();

} // namespace bdg::wish
