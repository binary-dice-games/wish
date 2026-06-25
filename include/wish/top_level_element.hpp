// MIT License © 2025 Binary Dice Games
/// @file top_level_element.hpp
/// @brief Common interface for top-level renderable elements that receive
///        widget events from their subtree.
#pragma once

#include "src/bison/bison_object.hpp"

namespace bdg::wish {

/// @brief Mixin for objects that own a renderable top-level subtree.
///
/// Both `ui_element` (for template-instantiated windows) and `form` (for
/// server-side forms) inherit this interface so the render loop can dispatch
/// widget events to whichever root element owned the triggering widget.
///
/// The default implementation is a no-op; subclasses override to react.
class top_level_element {
 public:
  /// @brief Called after each frame for every event fired by a descendant widget.
  ///
  /// @param widget_id   The `__wish_id` of the widget that fired the event.
  /// @param event_name  Event key (e.g. `"clicked"_key`, `"changed"_key`).
  /// @param payload     Event-specific payload fields (may be empty).
  virtual void on_event(bison::key_t widget_id,
                        bison::key_t event_name,
                        const bison::dynamic& payload) {}

  virtual ~top_level_element() = default;
};

}  // namespace bdg::wish
