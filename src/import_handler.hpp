// MIT License © 2025 Binary Dice Games
/// @file import_handler.hpp
/// @brief Server-side __WishImport RMI object that parses UI descriptors.
#pragma once

#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>
#include <string>

namespace bdg::wish {

/**
 * @brief Parse @p descriptor, register all resulting elements in @p ctx and
 *        @p sess, and return an indexed dynamic mapping position → {name, id}.
 *
 * Shared by both import_handler and template_handler.  Format is auto-detected
 * from the first non-whitespace character ('{' or '[' → JSON, otherwise YAML).
 *
 * @throws std::runtime_error on parse failure or unknown element type.
 */
bison::dynamic apply_descriptor(
    bison::rmi::context& ctx,
    session& sess,
    const std::string& descriptor);

/**
 * @brief Server-side bison dynamic that implements the `__WishImport` RMI
 *        class.
 *
 * One instance is created per client `OP_INSTANTIATE("wish", "__WishImport")`
 * call via `wish::server::on_create_object`.  Holds a reference to the bison
 * session context and a shared_ptr to the wish session so that `import()`
 * can register created objects in both the RMI object table and the wish
 * session's name map.
 */
class import_handler : public bison::dynamic {
 public:
  import_handler(
      bison::dynamic&& base,
      bison::rmi::context& ctx,
      std::shared_ptr<session> sess);

  /// @brief Register the `__WishImport` class in the `"wish"` namespace.
  static void register_class();

 private:
  bison::rmi::context& ctx_;
  std::shared_ptr<session> sess_;
};

}  // namespace bdg::wish
