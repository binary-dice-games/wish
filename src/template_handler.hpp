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
 * @brief Server-side bison dynamic that implements the `__WishTemplate` RMI
 *        class.
 *
 * Exposes two methods:
 * - `"register"_key(name, descriptor)` — stores a named descriptor in
 *   `session.templates`.
 * - `"instantiate"_key(name)` — looks up the stored descriptor and imports
 *   it, exactly like `import_handler::import()`.
 */
class template_handler : public bison::dynamic {
 public:
  template_handler(
      bison::dynamic&& base,
      bison::rmi::context& ctx,
      std::shared_ptr<session> sess);

  /// @brief Register the `__WishTemplate` class in the `"wish"` namespace.
  static void register_class();

 private:
  bison::rmi::context& ctx_;
  std::shared_ptr<session> sess_;
};

}  // namespace bdg::wish
