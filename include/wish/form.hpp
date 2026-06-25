// MIT License © 2025 Binary Dice Games
/// @file form.hpp
/// @brief Base class for all wish high-level form objects.
#pragma once

#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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

  /// @brief Remove this form's internal UI tree from session.objects.
  virtual ~form();

  /// @brief Inject session context.
  ///
  /// Called exactly once by server::on_create_object after the object is
  /// created and before it is accessible via RMI. Calls on_init() after
  /// storing ctx and sync_sess.
  ///
  /// @param ctx       Per-session RMI context; must outlive *this.
  /// @param sync_sess Synchronized wish session; held for the form's lifetime.
  void init(bison::rmi::context& ctx, sync_session_ptr sync_sess);

 protected:
  /// @brief Build the internal ui_element tree and register event handlers.
  ///
  /// Called from init() after ctx_ and sess_ are set. ctx() and sess() are
  /// valid for the lifetime of this form.
  virtual void on_init() = 0;

  /// @brief Register a server-side handler for events on an internal widget.
  ///
  /// Inserts an entry into `session.widget_event_handlers` keyed by
  /// `widget_id`.  The renderer's `enqueue_event()` will route events for
  /// this widget through the handler instead of delivering them to the client.
  ///
  /// Must be called from on_init() (i.e. within dispatch context).
  /// @param widget_id  The `__wish_id` of the internal widget to intercept.
  /// @param handler    Called with `(event_name, payload)` after the frame.
  void register_widget_handler(
      bison::key_t widget_id,
      std::function<void(bison::key_t, bison::dynamic)> handler);

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

  /// @return Reference to the wish session for the current dispatch context.
  ///
  /// Valid only while called from on_init(), on_set(), or any RMI handler
  /// (i.e. when `detail::current_session` is set by the dispatch hook).
  /// Do NOT call outside of dispatch — use sync_sess_->rlock() / wlock() for
  /// any access that must happen outside the dispatch stack.
  /// @throws std::logic_error if called outside a dispatch context.
  session& sess() {
    if (!detail::current_session)
      throw std::logic_error(
          "wish::form::sess() called outside RMI dispatch — "
          "use sync_sess_->rlock()/wlock() instead");
    return *detail::current_session;
  }

  /// @brief Key under which the internal Window root is stored in session.objects.
  ///
  /// Set by on_init() implementations. Used for cleanup when the form is
  /// closed or destroyed.
  std::string internal_root_key_;

  /// @brief Remove all session.objects entries owned by this form.
  ///
  /// Erases the root key and every key with the `"<root>."` prefix. Safe to
  /// call more than once (subsequent calls are no-ops). Also called by ~form().
  void remove_internal_objects();

 protected:
  /// Synchronized session wrapper; held for the form's lifetime.  Use
  /// sess() to access session data within dispatch; use sync_sess_->wlock()
  /// directly for access outside dispatch (e.g. destructor cleanup paths).
  sync_session_ptr sync_sess_;

 private:
  bison::rmi::context* ctx_{nullptr};
  /// Lazily resolved on the first call to emit().
  bison::key_t own_id_;
  /// IDs of widgets registered via register_widget_handler; cleared in
  /// remove_internal_objects.
  std::vector<bison::hash_t> registered_widget_ids_;
};

}  // namespace bdg::wish
