// MIT License © 2025 Binary Dice Games
/// @file ui_root.hpp
/// @brief Common base for top-level renderable objects that receive widget events.
#pragma once

#include <wish/ui_element.hpp>

namespace bdg::wish {

/// @brief Intermediate base for wish objects that own a renderable top-level subtree.
///
/// Sits between `ui_element` and concrete root types in the single-inheritance
/// chain: `bison::dynamic ← ui_element ← ui_root ← {window, form}`.
///
/// The render loop dispatches widget events to the owning `ui_root` via
/// `on_event`.  The default implementation is a no-op; subclasses override
/// to react.
class ui_root : public ui_element {
 public:
  explicit ui_root(bison::dynamic&& base) : ui_element(std::move(base)) {}

  /// @brief Called after each frame for every event fired by a descendant widget.
  ///
  /// @param widget_id   The `__wish_id` of the widget that fired the event.
  /// @param event_name  Event key (e.g. `"clicked"_key`, `"changed"_key`).
  /// @param payload     Event-specific payload fields (may be empty).
  virtual void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) {}

  virtual ~ui_root() = default;
};

} // namespace bdg::wish
