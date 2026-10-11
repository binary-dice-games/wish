// MIT License © 2026 Binary Dice Games
/// @file git_settings.hpp
/// @brief git's settings (see modules/bdg/common/tool_settings.hpp), read by
///        its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::git_settings {

inline constexpr const char* kCategory = "bdg.dev.git";

/// @brief Commits loaded into the history graph (`git log -n`).
inline settings::setting_info max_commits() {
  return common::int_setting(
      kCategory,
      "bdg.dev.git.max_commits",
      "Commits in history",
      "Most recent commits shown in the history graph. Applies the next time git opens.",
      300,
      50,
      5000,
      50);
}

inline common::tool_settings all() {
  return {{kCategory, "git", "Repository browser.", "res/icons/commit.png", "#F05033", 0}, {max_commits()}};
}

} // namespace bdg::wish::git_settings
