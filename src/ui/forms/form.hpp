// MIT License © 2025 Binary Dice Games
/// @file form.hpp
/// @brief Base class for all wish high-level form objects.
#pragma once

#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
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

  /// @brief Redirect this form's own emit()ed events to @p sink, in-process,
  /// instead of over RMI to a remote client.
  ///
  /// Lets one form C++-construct another as a private internal dialog (see
  /// `instantiate_child_form()`) and observe its results directly, the same
  /// way a remote client would via `onEvent()` -- e.g. a confirm-kill dialog
  /// built from the built-in `MessageBox` form reporting which button was
  /// clicked back to the form that opened it. No-op for a form created
  /// normally via RMI `instantiate()` unless this is called.
  void set_local_result_sink(std::function<void(bison::key_t, const bison::dynamic&)> sink) {
    local_result_sink_ = std::move(sink);
  }

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
  /// same form class (e.g. two nano windows), while remaining
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
  /// Lets concurrent instances of the same form class (e.g. two nano
  /// windows) get distinct keys deterministically, instead of either
  /// colliding (a fixed literal) or being run-unstable (a pointer/address).
  /// Call from on_init() to compute internal_root_key_, before this form's
  /// own root is registered -- so the current instance never sees itself as
  /// already taken.
  ///
  /// @param prefix  Form-class-specific prefix, e.g. "__nano_".
  std::string next_available_key(const std::string& prefix);

  /// @brief Remove all session.objects entries owned by this form.
  ///
  /// Erases `internal_root_key_` and every extra internal root registered via
  /// `set_default_dock_layout()` (and their `"<root>."`-prefixed children).
  /// Safe to call more than once (subsequent calls are no-ops). Also called
  /// by ~form().
  void remove_internal_objects();

  /// @brief Register a built-in default docking arrangement for this form's
  /// windows.
  ///
  /// @p layout_root is a `DockLayout` element and its `DockSplit`/`DockArea`
  /// child tree -- build it with `bdg::wish::dock::layout()`
  /// (src/ui/dock_layout_spec.hpp). Each `DockArea`'s `windows` names a
  /// `Window`'s `__path__`, i.e. the root key each `build_*_window()`
  /// registered (`internal_root_key_` and the secondary roots a multi-window
  /// form stamps into `(*root)["__path__"]`).
  ///
  /// The arrangement is applied once by the renderer -- on the first run
  /// whose `imgui.ini` has no node for the target dockspace, or after the
  /// element's `version` increases -- then owned by `imgui.ini` like any user
  /// drag. A no-op at render time if the host provides no ambient dockspace
  /// and the element names no explicit `target`.
  ///
  /// Call once from `on_init()` after every window is registered. The
  /// `DockLayout` object is torn down automatically with the form
  /// (`remove_internal_objects()` / `~form()`).
  void set_default_dock_layout(ui_element_ptr layout_root);

  /// @brief Ask the internal Window rooted at @p root_key to close itself,
  /// via the hidden "__request_close__" field render_window()'s modal
  /// branch checks (see imgui_ui_renderer.cpp).
  ///
  /// Call this instead of removing @p root_key's objects directly on
  /// confirm/cancel: on_event() runs outside any ImGui frame, so it cannot
  /// call ImGui::CloseCurrentPopup() itself -- only render_window() can,
  /// from inside the popup's own Begin/End scope. Skipping that call and
  /// just erasing the objects (as remove_objects_at() alone would do)
  /// leaves ImGui's own popup stack thinking this modal's ID is still open
  /// forever. The actual removal should happen once the Window's own
  /// "closed" event confirms ImGui really closed it.
  ///
  /// Safe to call from within dispatch or from an on_event() handler
  /// (outside dispatch) -- mirrors remove_internal_objects()'s own
  /// dispatch/non-dispatch branching.
  ///
  /// @param root_key  Top-level key of the internal Window to close, e.g.
  ///                  internal_root_key_ or a subclass's own secondary
  ///                  internal root (see mc's confirm dialog).
  void request_close_at(const std::string& root_key);

  /// @brief Erase every session.objects entry rooted at @p root_key: the
  /// key itself and every key with the `"<root_key>."` prefix, plus its
  /// top_level_objects/top_level_handlers entries.
  ///
  /// Generalizes remove_internal_objects() to an arbitrary root key, for
  /// forms that own more than one internal top-level Window (e.g.
  /// mc's overwrite-confirmation dialog, tracked separately from
  /// internal_root_key_). remove_internal_objects() itself is just this
  /// called with internal_root_key_.
  ///
  /// @param root_key  Top-level key to erase; a no-op if empty.
  void remove_objects_at(const std::string& root_key);

  /// @brief Construct another `form`-derived class @p Child as a private,
  /// in-process child of this form: instantiate it, inject session context
  /// via `init()` (running its `on_init()`), invoke its `"__construct"`
  /// hook with @p construct_params if it has one, and route its `emit()`ed
  /// events to @p on_result via `set_local_result_sink()` instead of over
  /// RMI -- the mechanism behind e.g. a confirm dialog built from the
  /// built-in `MessageBox` form, or a read-only properties dialog built
  /// from `PropertiesDialog`, reused by a server-side form without a real
  /// client-side `instantiate()` round trip.
  ///
  /// Mirrors what `bison::rmi::server::handle_instantiate()` does for a
  /// remote `instantiate()` call, except the child is never added to
  /// `ctx().objects`: no remote client can address it, so its own
  /// `emit()`-ed "public" events would otherwise go nowhere -- `on_result`
  /// is the only way they reach the caller.
  ///
  /// The returned child manages its OWN internal Window/widgets exactly as
  /// it would for a real client (its own `on_init()`, `on_event()`,
  /// `request_close_at()`), including tearing itself down on close. The
  /// caller only needs to keep the returned pointer alive for as long as
  /// the dialog should exist (e.g. as a member); replacing a stale,
  /// still-open instance with a fresh one (the caller simply overwriting
  /// its own member) is safe and mirrors how `top.cpp`/`mc.cpp`/etc. have
  /// always replaced a stale raw-element dialog -- `~form()` tears down
  /// the old instance's internal objects the same way `remove_objects_at()`
  /// would have.
  ///
  /// Safe to call from within RMI dispatch (e.g. an RMI method handler like
  /// `do_report_process_details()`) or from `on_event()` (outside it, like
  /// `form::request_close_at()`/`remove_objects_at()`): `child->init()` --
  /// and everything it runs (`on_init()`, `"__construct"`, which both
  /// typically need `sess()`) -- requires `detail::current_context` to be
  /// set, so when called outside dispatch this temporarily installs the
  /// current session as the active dispatch context for the duration of the
  /// call, mirroring the exact `context_wlock` + `detail::current_context =
  /// &*sess; ...; detail::current_context = nullptr;` idiom
  /// `server::render_frame()` (server.cpp) already uses to run
  /// dispatch-context-dependent code (there, rendering) from outside a real
  /// RMI dispatch.
  ///
  /// @tparam Child            Concrete `form` subclass to instantiate.
  /// @param klass              Registered `"wish"`-namespace class name.
  /// @param construct_params   Passed to the child's `"__construct"` hook,
  ///                           if it has one.
  /// @param on_result          Invoked in-process for every event the
  ///                           child `emit()`s (e.g. `MessageBox`'s
  ///                           `"on_result"`); may be empty if the child
  ///                           never needs to report anything back (e.g. a
  ///                           purely-informational `PropertiesDialog`).
  template <typename Child>
  std::shared_ptr<Child> instantiate_child_form(
      bison::key_t klass, bison::dynamic construct_params = {},
      std::function<void(bison::key_t, const bison::dynamic&)> on_result = {}) {
    static_assert(std::is_base_of_v<form, Child>, "instantiate_child_form: Child must derive from wish::form");
    auto child = bison::dynamic::instantiate<Child>(bison::key_t{bison::hash("wish")}, klass);
    child->set_local_result_sink(std::move(on_result));

    const bison::key_t construct_hook{bison::hash("__construct")};
    auto do_init = [&] {
      child->init(*ctx_, sync_ctx_);
      if (child->findMethod(construct_hook))
        child->call(construct_hook, construct_params);
    };

    if (detail::current_context) {
      do_init();
    } else {
      auto sess = context_wlock{*sync_ctx_};
      detail::current_context = &*sess;
      do_init();
      detail::current_context = nullptr;
    }
    return child;
  }

 protected:
  /// Synchronized session wrapper; held for the form's lifetime.  Use
  /// sess() to access session data within dispatch; use
  /// context_wlock{*sync_ctx_} directly for access outside dispatch (e.g.
  /// destructor cleanup paths).
  sync_context_ptr sync_ctx_;

 private:
  bison::rmi::context* ctx_{nullptr};
  /// Extra internal top-level roots (beyond `internal_root_key_`) this form
  /// registered -- currently just the `set_default_dock_layout()` object.
  /// Cleared by `remove_internal_objects()`.
  std::vector<std::string> extra_internal_roots_;
  /// Lazily resolved on the first call to emit().
  bison::key_t own_id_;
  /// Set by set_local_result_sink(); when present, emit() routes through it
  /// instead of over RMI. See set_local_result_sink()'s doc comment.
  std::function<void(bison::key_t, const bison::dynamic&)> local_result_sink_;
};

} // namespace bdg::wish
