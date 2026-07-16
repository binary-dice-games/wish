// MIT License © 2025 Binary Dice Games
/// @file message_box.hpp
/// @brief Server-side MessageBox form class.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <string>
#include <unordered_map>

namespace bdg::wish {

/// @brief High-level modal message dialog, modeled on the Win32 MessageBox API.
///
/// Shows a title, a body message, an optional icon (`icon`: "none", "info",
/// "warning", "error", "question"), and a Win32-style button preset (`buttons`:
/// "ok", "ok_cancel", "yes_no", "yes_no_cancel", "retry_cancel", or
/// "abort_retry_ignore"). The internal Window is rendered as a true
/// input-blocking modal popup (Window.modal = true) and has no title-bar close
/// button — the user must click one of the presented buttons.
///
/// Clicking any button emits a single `on_result` event with a `button` field
/// holding one of "ok", "cancel", "yes", "no", "retry", "abort", "ignore" (the
/// name of the clicked button), then removes the internal UI tree.
class message_box : public form {
 public:
  explicit message_box(bison::dynamic&& base);

  /// @brief Called from the `__construct` prototype method, once, right
  /// after instantiation -- see `bison::rmi::server::handle_instantiate()`.
  ///
  /// `on_init()` (and therefore the default "ok"-only tree) has already run
  /// by this point, since bison calls `__construct` only after the object
  /// exists. Applies @p params onto this object's own fields (same effect
  /// as a client set() call, via the same field-patch idiom
  /// `server::handle_set()` uses for `__setter`), then tears down and
  /// rebuilds the internal tree so a `buttons`/`icon` preset supplied at
  /// `instantiate(..., params)` time actually takes effect -- construction
  /// params otherwise never reach on_init() and would silently do nothing.
  void on_construct(const bison::dynamic& params);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Build (or rebuild) the internal Window tree from this object's
  /// current title/message/icon/buttons fields. Idempotent: safe to call
  /// again after remove_internal_objects(). Does not itself register the
  /// result as a top-level object -- see register_root().
  void rebuild();

  /// @brief Register the tree built by rebuild() as this form's top-level
  /// root (mirrors form::init()'s own post-on_init() registration, in
  /// form.cpp). form::init() already does this once automatically after
  /// on_init() returns; on_construct() must redo it explicitly for a
  /// rebuild that happens after form::init() has already completed.
  void register_root();

  /// @brief Ask the internal Window to close itself, via the hidden
  /// "__request_close__" field render_window()'s modal branch checks (see
  /// imgui_ui_renderer.cpp).
  ///
  /// Called instead of remove_internal_objects() directly on a button
  /// click: on_event() runs outside any ImGui frame, so it cannot call
  /// ImGui::CloseCurrentPopup() itself -- only render_window() can, from
  /// inside the popup's own Begin/End scope. Skipping that call and just
  /// removing the Window from top_level_objects (as remove_internal_objects()
  /// alone would do) leaves ImGui's own popup stack thinking this modal's ID
  /// is still open forever; the *next* MessageBox instance, whose
  /// internal_root_key_ likely recycles this same freed key (see
  /// form::next_available_key()), then collides with that stale entry and
  /// fails to open until an unrelated input event forces ImGui to
  /// reconcile its stack. The actual removal happens once the Window's own
  /// "closed" event confirms ImGui really closed it -- see on_event()'s
  /// window_id_ branch.
  void request_close();

  /// @brief Widget ID → result string ("ok", "cancel", "yes", ...) for every
  /// button in the currently selected preset.
  std::unordered_map<bison::key_t, std::string, bison::key_t, bison::key_t> button_results_;

  /// @brief __wish_id of the internal root Window, cached so on_event() can
  /// tell its "closed" event (fired once ImGui confirms the modal actually
  /// closed) apart from a button's "clicked" event.
  bison::key_t window_id_;
};

/// @brief Register MessageBox in the "wish" bison namespace.
void register_message_box();

} // namespace bdg::wish
