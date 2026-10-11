// MIT License © 2026 Binary Dice Games
/// @file curl_settings.hpp
/// @brief curl's settings (see modules/bdg/common/tool_settings.hpp), shared
///        by its server form and its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::curl_settings {

inline constexpr const char* kCategory = "bdg.dev.curl";

/// @brief The toolbar's Follow redirects checkbox (server form); saved when
///        toggled.
inline settings::setting_info follow_redirects() {
  return common::bool_setting(
      kCategory,
      "bdg.dev.curl.follow_redirects",
      "Follow redirects",
      "Follows 3xx responses to their new location (curl -L).",
      true);
}

/// @brief curl's --max-time (client), read at each request.
inline settings::setting_info timeout() {
  return common::int_setting(
      kCategory,
      "bdg.dev.curl.timeout",
      "Request timeout",
      "Seconds a request may take before it is cancelled; 0 waits forever.",
      0,
      0,
      600,
      5);
}

/// @brief Requests kept in the History list (client).
inline settings::setting_info history_size() {
  return common::int_setting(
      kCategory,
      "bdg.dev.curl.history_size",
      "History size",
      "Requests kept in the History list. Applies the next time curl opens.",
      200,
      10,
      1000,
      10);
}

inline common::tool_settings all() {
  return {
      {kCategory, "curl", "HTTP client.", "res/icons/send.png", "#FF2D55", 0},
      {follow_redirects(), timeout(), history_size()}};
}

} // namespace bdg::wish::curl_settings
