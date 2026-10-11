// MIT License © 2026 Binary Dice Games
/// @file sq_settings.hpp
/// @brief sq's settings (see modules/bdg/common/tool_settings.hpp).
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::sq_settings {

inline constexpr const char* kCategory = "bdg.dev.sq";

/// @brief The SQL editor's row limit combo (server form); saved when changed.
/// The values are sq.cpp's kRowLimits.
inline settings::setting_info row_limit() {
  return common::choice_setting(
      kCategory,
      "bdg.dev.sq.row_limit",
      "Row limit",
      "Most rows a query returns (the SQL editor's row limit).",
      "500",
      {{"100", "100 rows"}, {"500", "500 rows"}, {"1000", "1000 rows"}, {"5000", "5000 rows"}});
}

inline common::tool_settings all() {
  return {{kCategory, "sq", "SQL database browser.", "res/icons/database.png", "#30B0C7", 0}, {row_limit()}};
}

} // namespace bdg::wish::sq_settings
