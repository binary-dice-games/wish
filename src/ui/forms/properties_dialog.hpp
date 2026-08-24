// MIT License © 2025 Binary Dice Games
/// @file properties_dialog.hpp
/// @brief Server-side PropertiesDialog form class.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <string>

namespace bdg::wish {

/// @brief Read-only "Properties" dialog: a title bar, an `ObjectInspector`
/// (`src/ui/ui_elements/object_inspector.hpp`) reflecting over `target`, and
/// a single Close button.
///
/// Built-in reusable counterpart to `MessageBox` for the "show a grid of an
/// object's fields" dialog every file-manager-style tool otherwise
/// re-implements by hand as a fixed set of `Label` rows (see e.g. `top`'s
/// process-properties dialog, `mc`'s file-properties dialog): set `target`
/// to any bison object whose class is registered in this process (see
/// `object_inspector.hpp`'s "Reflection is process-local" note), and every
/// visible field renders as a read-only row automatically, keyed off the
/// same `DisplayName`/`Description`/`Order` attributes the class was
/// registered with -- no per-field UI code, and no separate "how do I
/// display this field" logic to keep in sync with the class definition.
///
/// The internal `ObjectInspector` is always built with `read_only: true`
/// and `show_description_panel: false` -- this form is a pure viewer, not
/// an editor, and the extra description panel has no room to earn its
/// keep in a dialog this small.
///
/// Like `MessageBox`, this can be instantiated remotely by a client (over
/// RMI) or privately by another server-side `form` via
/// `form::instantiate_child_form<properties_dialog>(...)` -- the latter is
/// how `top`/`mc` show their own process/file properties dialogs without
/// reimplementing this layout. There is no `on_result` event to listen for
/// (a properties view has nothing to report back); the dialog just closes
/// itself when its own Close button or the window's own X button is
/// clicked.
class properties_dialog : public form {
 public:
  explicit properties_dialog(bison::dynamic&& base);

  /// @brief Called from the `__construct` prototype method, once, right
  /// after instantiation -- see `message_box::on_construct()`'s doc comment
  /// for why construction-time params otherwise never reach `on_init()`.
  /// Applies @p params onto this object's own fields, then rebuilds.
  void on_construct(const bison::dynamic& params);

  /// @brief Replace the object this dialog reflects over, rebuilding the
  /// inspector's rows in place -- safe to call on an already-open dialog
  /// (e.g. once an async details response arrives after the dialog was
  /// first opened with a `nullptr`/placeholder target).
  void set_target(bison::dynamic_ptr target);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Build (or rebuild) the internal Window tree from this object's
  /// current title/target fields. Idempotent: safe to call again after
  /// remove_internal_objects().
  void rebuild();

  /// @brief Register the tree built by rebuild() as this form's top-level
  /// root -- see message_box::register_root()'s doc comment for why a
  /// rebuild after form::init() has already run must redo this explicitly.
  void register_root();

  /// @brief Release the internal `ObjectInspector`'s own previously-built
  /// rows (see `object_inspector::release()`'s doc comment for why this
  /// can't be automatic) and clear `inspector_elem_`. Safe to call more
  /// than once, and safe to call even if nothing was ever built. Must run
  /// before tearing down/rebuilding the tree that owns the `ObjectInspector`
  /// element itself, or its rows -- stored at their own independent
  /// `ui_objects` paths, not nested under this form's own root -- would
  /// leak.
  ///
  /// Deliberately NOT called from a destructor -- exactly like
  /// `object_inspector::release()` itself, and for the same reason (see its
  /// doc comment): this instance may be destroyed as a side effect of whole-
  /// session teardown (`bison::rmi::context::objects.clear()`), and erasing
  /// further `ctx.objects` entries (which `object_inspector::release()`
  /// does) while that same map is being cleared is undefined behavior.
  /// Only call this from a still-alive-session path: rebuild() (retargeting
  /// an already-open dialog) or on_event()'s graceful window-close handling.
  void release_inspector();

  bison::key_t window_id_;
  bison::key_t close_id_;

  /// The `ObjectInspector` element spliced into "vbox.inspector" -- kept
  /// live so set_target()/release_inspector() can reach it without a path
  /// lookup. `ui_element_ptr`, not `bison::dynamic_ptr`, to match the type
  /// `context::ui_objects` (a `name_map`) itself stores.
  ui_element_ptr inspector_elem_;

  /// The object currently reflected over; mirrors this form's own `target`
  /// field so set_target() can rebuild the inspector without needing a
  /// dispatch-only field read.
  bison::dynamic_ptr target_;
};

/// @brief Register PropertiesDialog in the "wish" bison namespace.
void register_properties_dialog();

} // namespace bdg::wish
