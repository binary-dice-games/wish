// MIT License © 2025 Binary Dice Games
/// @file form.hpp
/// @brief Base class for all wish high-level form objects.
#pragma once

#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>
#include <string>

namespace bdg::wish {

/// @brief Base class for all wish form objects.
///
/// A form is a bison RMI object (not a ui_element) that manages an internal
/// ui_element tree on behalf of the server. The client interacts only with the
/// form's declared fields, methods, and events — the internal widgets are
/// private to the form.
///
/// The server calls init() once after the object is created to supply
/// per-session context. Subclasses override on_init() to build their internal
/// UI and register prototype event/field handlers.
///
/// File access inside a form must go through file_service::resolve_path()
/// or resolve_widget_path() — the same sandbox rules as low-level elements.
class form : public bison::dynamic {
 public:
  explicit form(bison::dynamic&& base);

  /// @brief Inject session context.
  ///
  /// Called exactly once by server::on_create_object after the object is
  /// created and before it is accessible via RMI. Calls on_init() after
  /// storing ctx and sess.
  ///
  /// @param ctx  Per-session RMI context; must outlive *this.
  /// @param sess Shared wish session state.
  void init(bison::rmi::context& ctx, std::shared_ptr<session> sess);

 protected:
  /// @brief Build the internal ui_element tree and register event handlers.
  ///
  /// Called from init() after ctx_ and sess_ are set. ctx() and sess() are
  /// valid for the lifetime of this form.
  virtual void on_init() = 0;

  /// @brief Emit a high-level event to the client.
  ///
  /// Locates the form's own RMI object ID via a one-time scan of
  /// ctx().objects and forwards to sess().emit_event. Safe to call after
  /// init() returns; must not be called before init().
  ///
  /// @param event_name  Hashed event key (e.g. "on_open"_key).
  /// @param payload     Optional event payload fields.
  void emit(bison::key_t event_name, bison::dynamic payload = {});

  /// @return Reference to the per-session RMI context supplied by init().
  bison::rmi::context& ctx() { return *ctx_; }

  /// @return Reference to the wish session supplied by init().
  session& sess() { return *sess_; }

  /// @brief Key under which the internal Window root is stored in session.objects.
  ///
  /// Set by on_init() implementations. Used for cleanup when the form is
  /// closed or destroyed.
  std::string internal_root_key_;

 private:
  bison::rmi::context*     ctx_{nullptr};
  std::shared_ptr<session> sess_;
  /// Lazily resolved on the first call to emit().
  bison::key_t             own_id_;
};

}  // namespace bdg::wish
