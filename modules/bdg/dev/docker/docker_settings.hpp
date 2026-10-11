// MIT License © 2026 Binary Dice Games
/// @file docker_settings.hpp
/// @brief docker's settings (see modules/bdg/common/tool_settings.hpp),
///        shared by its server form and its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::docker_settings {

inline constexpr const char* kCategory = "bdg.dev.docker";
inline constexpr const char* kLogs = "Logs";

/// @brief The Logs window's Follow checkbox (server form); saved when toggled.
inline settings::setting_info logs_follow() {
  return common::bool_setting(kCategory, "bdg.dev.docker.logs_follow", "Follow logs",
                              "Keeps reading new log lines (the Logs window's Follow checkbox).", false, kLogs);
}

/// @brief The Logs window's Lines field (server form); saved when changed.
inline settings::setting_info logs_lines() {
  return common::int_setting(kCategory, "bdg.dev.docker.logs_lines", "Log lines",
                             "Lines read from the end of the log (the Logs window's Lines field).", 500, 10,
                             10000, 100, kLogs);
}

/// @brief Seconds between two samples of the Stats window (client).
inline settings::setting_info stats_interval() {
  return common::int_setting(kCategory, "bdg.dev.docker.stats_interval", "Stats interval",
                             "Seconds between two samples of the Stats graphs. Applies the next time docker "
                             "opens.",
                             3, 1, 300);
}

inline common::tool_settings all() {
  return {{kCategory, "docker", "Containers, images, volumes and networks.", "res/icons/container.png", "#1D63ED", 0},
          {logs_follow(), logs_lines(), stats_interval()}};
}

} // namespace bdg::wish::docker_settings
