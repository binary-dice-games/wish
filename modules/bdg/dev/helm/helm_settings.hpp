// MIT License © 2026 Binary Dice Games
/// @file helm_settings.hpp
/// @brief helm's settings (see modules/bdg/common/tool_settings.hpp): the
///        Install dialog's options, which the dialog starts with and saves
///        when they change.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::helm_settings {

inline constexpr const char* kCategory = "bdg.dev.helm";
inline constexpr const char* kInstall = "Install";

inline settings::setting_info create_namespace() {
  return common::bool_setting(
      kCategory,
      "bdg.dev.helm.create_namespace",
      "Create namespace",
      "Creates the release's namespace when it doesn't exist (--create-namespace).",
      true,
      kInstall);
}

inline settings::setting_info wait() {
  return common::bool_setting(
      kCategory,
      "bdg.dev.helm.wait",
      "Wait",
      "Waits until the release's resources are ready before reporting success (--wait).",
      false,
      kInstall);
}

inline common::tool_settings all() {
  return {{kCategory, "helm", "Kubernetes charts.", "res/icons/helm.png", "#0F1689", 0}, {create_namespace(), wait()}};
}

} // namespace bdg::wish::helm_settings
