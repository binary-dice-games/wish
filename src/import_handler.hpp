// MIT License © 2025 Binary Dice Games
/// @file import_handler.hpp
/// @brief Server-side __WishImport RMI object that parses UI descriptors.
#pragma once

#include "wish_handler.hpp"

namespace bdg::wish {

/**
 * @brief Shared utility: parse @p descriptor, register elements in @p ctx
 *        and @p sess, and return an indexed result payload.
 *
 * Format is auto-detected from the first non-whitespace character
 * ('{' or '[' → JSON, otherwise YAML).
 *
 * @throws std::runtime_error on parse failure or unknown element type.
 */
bison::dynamic apply_descriptor(
    bison::rmi::context& ctx,
    session& sess,
    const std::string& descriptor);

/**
 * @brief Server-side bison dynamic for the `__WishImport` RMI class.
 *
 * Registered via `import_handler::register_class()` as the prototype for
 * `__WishImport` in the `"wish"` namespace.  `clone_for_instance()` creates
 * a fresh `import_handler`; `wish::server::on_create_object` then calls
 * `init()` to supply session context.
 */
class import_handler : public wish_handler {
 public:
  explicit import_handler(bison::dynamic&& base);

  /// @brief Register `import_handler` as the `__WishImport` class prototype.
  static void register_class();

  bison::dynamic_ptr clone_for_instance() const override;
};

}  // namespace bdg::wish
