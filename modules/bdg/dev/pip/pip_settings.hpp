// MIT License © 2026 Binary Dice Games
/// @file pip_settings.hpp
/// @brief pip's settings (see modules/bdg/common/tool_settings.hpp): the
///        install bar's options, which the form starts with and saves when
///        they change.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::pip_settings {

inline constexpr const char* kCategory = "bdg.dev.pip";
inline constexpr const char* kInstall = "Install";

inline settings::setting_info upgrade() {
  return common::bool_setting(kCategory, "bdg.dev.pip.upgrade", "Upgrade",
                              "Upgrades packages that are already installed (pip install --upgrade).", false,
                              kInstall);
}

inline settings::setting_info user() {
  return common::bool_setting(kCategory, "bdg.dev.pip.user", "User install",
                              "Installs into your user site-packages (pip install --user).", false, kInstall);
}

inline settings::setting_info pre() {
  return common::bool_setting(kCategory, "bdg.dev.pip.pre", "Pre-releases",
                              "Includes pre-release and development versions (pip install --pre).", false, kInstall);
}

inline common::tool_settings all() {
  return {{kCategory, "pip", "Python packages.", "res/icons/download.png", "#3776AB", 0}, {upgrade(), user(), pre()}};
}

} // namespace bdg::wish::pip_settings
