// MIT License © 2026 Binary Dice Games
/// @file mc_settings.hpp
/// @brief mc's settings (see modules/bdg/common/tool_settings.hpp). The form
///        reads them each time it lists a folder.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::mc_settings {

inline constexpr const char* kCategory = "bdg.desktop.mc";

inline settings::setting_info show_hidden() {
  return common::bool_setting(kCategory, "bdg.desktop.mc.show_hidden", "Show hidden files",
                              "Lists files and folders whose name starts with a dot.", true);
}

inline settings::setting_info folders_first() {
  return common::bool_setting(kCategory, "bdg.desktop.mc.folders_first", "Keep folders on top",
                              "Lists folders before files, whatever the sort column.", false);
}

inline common::tool_settings all() {
  return {{kCategory, "mc", "File Explorer.", "res/icons/folder_open.png", "#0A84FF", 0},
          {show_hidden(), folders_first()}};
}

} // namespace bdg::wish::mc_settings
