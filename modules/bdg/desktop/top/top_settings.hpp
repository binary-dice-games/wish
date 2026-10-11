// MIT License © 2026 Binary Dice Games
/// @file top_settings.hpp
/// @brief top's settings (see modules/bdg/common/tool_settings.hpp), shared
///        by its server form and its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::top_settings {

inline constexpr const char* kCategory = "bdg.desktop.top";

/// @brief Milliseconds between samples (client). Read when top starts.
inline settings::setting_info refresh_ms() {
  return common::int_setting(kCategory, "bdg.desktop.top.refresh_ms", "Refresh interval",
                             "Milliseconds between two samples of the process list. Applies the next "
                             "time top opens.",
                             1000, 250, 10000, 250);
}

/// @brief Samples kept in the CPU and memory graphs (server form).
inline settings::setting_info history() {
  return common::int_setting(kCategory, "bdg.desktop.top.history", "Graph history",
                             "Samples shown in the CPU and memory graphs. Applies the next time top opens.",
                             60, 10, 600, 10);
}

inline common::tool_settings all() {
  return {{kCategory, "top", "System monitor.", "res/icons/monitoring.png", "#34C759", 0},
          {refresh_ms(), history()}};
}

} // namespace bdg::wish::top_settings
