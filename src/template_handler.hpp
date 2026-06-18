// MIT License © 2025 Binary Dice Games
/// @file template_handler.hpp
/// @brief Server-side __WishTemplate RMI object for named UI templates.
#pragma once

#include "wish_handler.hpp"

namespace bdg::wish {

/**
 * @brief Server-side bison dynamic for the `__WishTemplate` RMI class.
 *
 * Exposes two methods:
 * - `"register"_key(name, descriptor)` — stores a named descriptor in the
 *   session's template map.
 * - `"instantiate"_key(name)` — looks up the stored descriptor and imports
 *   it, exactly like `import_handler::import()`.
 *
 * Registered as the concrete prototype for `__WishTemplate` so that
 * `bison::dynamic::create_instance` produces a `template_handler` directly.
 */
class template_handler : public wish_handler {
 public:
  explicit template_handler(bison::dynamic&& base);
  bison::dynamic_ptr clone_for_instance() const override;
};

/// @brief Register `template_handler` as the `__WishTemplate` class prototype.
void register_template_handler();

}  // namespace bdg::wish
