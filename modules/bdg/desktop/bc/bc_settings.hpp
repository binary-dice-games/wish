// MIT License © 2026 Binary Dice Games
/// @file bc_settings.hpp
/// @brief bc's settings (see modules/bdg/common/tool_settings.hpp). The form
///        starts with them and saves them when they change from the View
///        menu or the mode tabs.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::bc_settings {

inline constexpr const char* kCategory = "bdg.desktop.bc";

inline settings::setting_info mode() {
  return common::choice_setting(
      kCategory,
      "bdg.desktop.bc.mode",
      "Mode",
      "Keypad bc opens with.",
      "standard",
      {{"standard", "Standard"}, {"scientific", "Scientific"}, {"programmer", "Programmer"}});
}

inline settings::setting_info angle_unit() {
  return common::choice_setting(
      kCategory,
      "bdg.desktop.bc.angle_unit",
      "Angle unit",
      "Unit of the trigonometric functions in scientific mode.",
      "degrees",
      {{"degrees", "Degrees"}, {"radians", "Radians"}, {"gradians", "Gradians"}});
}

inline settings::setting_info digit_grouping() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.bc.digit_grouping",
      "Digit grouping",
      "Separates thousands (and binary/hex digit groups) in the display.",
      true);
}

inline settings::setting_info show_history() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.bc.show_history",
      "Show history",
      "Shows the panel of past calculations next to the keypad.",
      false);
}

inline common::tool_settings all() {
  return {
      {kCategory, "bc", "Calculator.", "res/icons/calculate.png", "#FF9500", 0},
      {mode(), angle_unit(), digit_grouping(), show_history()}};
}

} // namespace bdg::wish::bc_settings
