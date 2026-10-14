// MIT License © 2026 Binary Dice Games
/// @file dock_frame.hpp
/// @brief Test helper: finds the app frame (the `DockSpaceViewport` a form
///        registers with `form::set_default_dock_layout()`). A tool's panels
///        have no close button; closing the frame closes the tool.
#pragma once

#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"

#include <string>

namespace wish_test {

/// @brief Root key (`"__docklayout_<n>"`) of the first app frame in @p s;
///        empty when there is none.
inline std::string dock_frame_root(const bdg::wish::context& s) {
  for (const auto& [key, obj] : s.ui_objects)
    if (key.rfind("__docklayout_", 0) == 0 && obj && obj->class_key() == bdg::bison::key_t{"DockSpaceViewport"})
      return key;
  return {};
}

/// @brief `__wish_id` of the first app frame in @p s (the id its `closed`
///        event carries); id 0 when there is none.
inline bdg::bison::key_t dock_frame_id(const bdg::wish::context& s) {
  const std::string root = dock_frame_root(s);
  return root.empty() ? bdg::bison::key_t{} : s.ui_objects.at(root)->as<bdg::bison::key_t>(bdg::bison::key_t{"__wish_id"});
}

/// @brief Delivers the first app frame's `closed` event to its owner, as
///        clicking its title-bar X does. Returns false when @p s has no
///        frame or the frame has no owner.
inline bool close_dock_frame(bdg::wish::context& s) {
  const std::string root = dock_frame_root(s);
  auto h = s.top_level_handlers.find(bdg::bison::key_t{root});
  if (root.empty() || h == s.top_level_handlers.end() || !h->second)
    return false;
  h->second->on_event(dock_frame_id(s), bdg::bison::key_t{"closed"}, bdg::bison::dynamic{});
  return true;
}

} // namespace wish_test
