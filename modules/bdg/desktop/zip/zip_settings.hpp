// MIT License © 2026 Binary Dice Games
/// @file zip_settings.hpp
/// @brief zip's settings (see modules/bdg/common/tool_settings.hpp), read
///        by its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::zip_settings {

inline constexpr const char* kCategory = "bdg.desktop.zip";

/// @brief Deflate level of new archives, read at each compression.
inline settings::setting_info compression_level() {
  return common::int_setting(
      kCategory,
      "bdg.desktop.zip.compression_level",
      "Compression level",
      "0 stores files uncompressed; 9 makes the smallest archives, slowest.",
      6,
      0,
      9);
}

inline common::tool_settings all() {
  return {{kCategory, "zip", "Archive manager.", "res/icons/zip.png", "#A2845E", 0}, {compression_level()}};
}

} // namespace bdg::wish::zip_settings
