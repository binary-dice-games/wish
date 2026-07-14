// MIT License © 2025 Binary Dice Games
/// @file form.hpp
/// @brief Base class for all wish high-level form objects.
#pragma once

#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <memory>
#include <stdexcept>
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
class form : public ui_root {
 public:
  explicit form(bison::dynamic&& base);

  /// @brief Remove this form's internal UI tree from session.objects.
  virtual ~form();

  /// @brief Inject session context.
  ///
  /// Called exactly once by server::on_create_object after the object is
  /// created and before it is accessible via RMI. Calls on_init() after
  /// storing ctx and sync_ctx.
  ///
  /// @param ctx      Per-session RMI context; must outlive *this.
  /// @param sync_ctx Synchronized wish session; held for the form's lifetime.
  void init(bison::rmi::context& ctx, sync_context_ptr sync_ctx);

 protected:
  /// @brief Build the internal ui_element tree and register event handlers.
  ///
  /// Called from init() after ctx_ and sess_ are set. ctx() and sess() are
  /// valid for the lifetime of this form.
  virtual void on_init() = 0;

  /// @brief React to a widget event from this form's internal subtree.
  ///
  /// Called by the render loop after each frame for every event fired by a
  /// widget that belongs to this form. The default implementation is a no-op;
  /// subclasses override to close dialogs, update fields, etc.
  ///
  /// Called outside the session lock — handlers may freely acquire
  /// `context_wlock{*sync_ctx_}` or modify session state.
  ///
  /// @param widget_id   The `__wish_id` of the widget that fired the event.
  /// @param event_name  Event key (e.g. `"clicked"_key`, `"changed"_key`).
  /// @param payload     Event payload (may be empty).
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override {}

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
  bison::rmi::context& ctx() {
    return *ctx_;
  }

  /// @return Reference to the wish session for the current dispatch context.
  ///
  /// Valid only while called from on_init(), on_set(), or any RMI handler
  /// (i.e. when `detail::current_context` is set by the dispatch hook).
  /// Do NOT call outside of dispatch — use context_rlock/context_wlock over
  /// sync_ctx_ for any access that must happen outside the dispatch stack.
  /// @throws std::logic_error if called outside a dispatch context.
  context& sess() {
    if (!detail::current_context)
      throw std::logic_error(
          "wish::form::sess() called outside RMI dispatch — "
          "use context_rlock/context_wlock over sync_ctx_ instead");
    return *detail::current_context;
  }

  /// @brief Key under which the internal Window root is stored in session.objects.
  ///
  /// Set by on_init() implementations -- via next_available_key(), NOT a
  /// pointer-derived string -- and used for cleanup when the form is closed
  /// or destroyed, and (by form::init(), after on_init() returns) as the
  /// root Window element's "__path__", which stable_id() in
  /// imgui_ui_renderer.hpp hashes into the ImGui id. next_available_key()
  /// guarantees this stays unique across concurrently-open instances of the
  /// same form class (e.g. two Notepad windows), while remaining
  /// deterministic run-to-run for a given open order/count -- unlike the
  /// pointer-derived id this replaced, which was both run-unstable AND,
  /// since ui_tree::merge() only prefixes ui_objects' *keys* and never
  /// touches this field, indistinguishable from another instance's empty
  /// "" path until next_available_key() existed.
  std::string internal_root_key_;

  /// @brief Returns "prefix0", "prefix1", ... -- the lowest-numbered
  /// candidate not already registered as a top-level object in the current
  /// session.
  ///
  /// Lets concurrent instances of the same form class (e.g. two Notepad
  /// windows) get distinct keys deterministically, instead of either
  /// colliding (a fixed literal) or being run-unstable (a pointer/address).
  /// Call from on_init() to compute internal_root_key_, before this form's
  /// own root is registered -- so the current instance never sees itself as
  /// already taken.
  ///
  /// @param prefix  Form-class-specific prefix, e.g. "__notepad_".
  std::string next_available_key(const std::string& prefix);

  /// @brief Remove all session.objects entries owned by this form.
  ///
  /// Erases the root key and every key with the `"<root>."` prefix. Safe to
  /// call more than once (subsequent calls are no-ops). Also called by ~form().
  void remove_internal_objects();

 protected:
  /// Synchronized session wrapper; held for the form's lifetime.  Use
  /// sess() to access session data within dispatch; use
  /// context_wlock{*sync_ctx_} directly for access outside dispatch (e.g.
  /// destructor cleanup paths).
  sync_context_ptr sync_ctx_;

 private:
  bison::rmi::context* ctx_{nullptr};
  /// Lazily resolved on the first call to emit().
  bison::key_t own_id_;
};

} // namespace bdg::wish
