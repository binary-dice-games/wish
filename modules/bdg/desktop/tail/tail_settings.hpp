// MIT License © 2026 Binary Dice Games
/// @file tail_settings.hpp
/// @brief tail's settings (see modules/bdg/common/tool_settings.hpp), shared
///        by its server form and its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::tail_settings {

inline constexpr const char* kCategory = "bdg.desktop.tail";

/// @brief Lines read from the end of each file when `-n` isn't given (client).
inline settings::setting_info lines() {
  return common::int_setting(
      kCategory,
      "bdg.desktop.tail.lines",
      "Initial lines",
      "Lines shown from the end of each file when tail is run without -n; the Lines field "
      "saves it.",
      100,
      1,
      2000,
      10);
}

/// @brief The toolbar's Follow checkbox (server form); saved when toggled.
inline settings::setting_info follow() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.tail.follow",
      "Scroll to new lines",
      "Keeps the newest line in view as lines arrive (the Follow checkbox).",
      true);
}

/// @brief Milliseconds between two checks of a file for new lines (client).
inline settings::setting_info poll_ms() {
  return common::int_setting(
      kCategory,
      "bdg.desktop.tail.poll_ms",
      "Check interval",
      "Milliseconds between two checks of each file for new lines. Applies the next time "
      "tail opens.",
      300,
      100,
      5000,
      100);
}

inline common::tool_settings all() {
  return {{kCategory, "tail", "Log viewer.", "res/icons/log.png", "#5856D6", 0}, {lines(), follow(), poll_ms()}};
}

} // namespace bdg::wish::tail_settings
