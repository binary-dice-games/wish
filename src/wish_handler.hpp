// MIT License © 2025 Binary Dice Games
/// @file wish_handler.hpp
/// @brief Base class for wish protocol handler objects that need session context.
#pragma once

#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>

namespace bdg::wish {

/**
 * @brief Base for server-side bison dynamic objects that hold wish session
 *        context.
 *
 * Subclasses (`import_handler`, `template_handler`) are registered as the
 * concrete prototype for their respective RMI class.  When the bison RMI
 * server calls `clone_for_instance()` to create a new object, it gets back
 * an instance of the right subclass.  `wish::server::on_create_object` then
 * calls `init()` once to inject the per-session context before the object is
 * handed to the client.
 */
class wish_handler : public bison::dynamic {
 public:
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

 protected:
  explicit wish_handler(bison::dynamic&& base) : dynamic(std::move(base)) {}

  bison::rmi::context* ctx_{nullptr};
  std::shared_ptr<session> sess_;
};

}  // namespace bdg::wish
