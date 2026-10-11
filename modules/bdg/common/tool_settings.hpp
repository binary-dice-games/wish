// MIT License © 2026 Binary Dice Games
/// @file tool_settings.hpp
/// @brief Builders for the settings metadata the bdg modules declare.
///
/// Each module defines its settings once, in a `<name>_settings.hpp` next
/// to its `server/` and `client/` directories, with these builders. Its
/// server registration function passes them to `register_tool_settings()`
/// so the Settings tool (bdg/desktop/conf) lists them for every identity;
/// its server form reads them with `settings::int_of()` & co. on
/// `sess().user_store`, and its client runner with
/// `wish_app_host::read_int_setting()` & co. See docs/settings.md.
#pragma once

#include <context/settings_schema.hpp>

#include <string>
#include <utility>
#include <vector>

namespace bdg::wish::common {

/// @brief A module's settings: its sidebar category and the settings in it.
struct tool_settings {
  settings::category_info category;
  std::vector<settings::setting_info> settings;
};

/// @brief A setting of @p category; @p name is "<entry>.<field>".
inline settings::setting_info make_setting(const std::string& category, const std::string& name,
                                           settings::setting_type type, const std::string& label,
                                           const std::string& description, const std::string& section = {}) {
  settings::setting_info s;
  s.name = name;
  s.category = category;
  s.type = type;
  s.label = label;
  s.description = description;
  s.section = section;
  return s;
}

/// @brief A switch.
inline settings::setting_info bool_setting(const std::string& category, const std::string& name,
                                           const std::string& label, const std::string& description,
                                           bool default_value, const std::string& section = {}) {
  auto s = make_setting(category, name, settings::setting_type::boolean, label, description, section);
  s.default_value = default_value;
  return s;
}

/// @brief An integer in [@p min, @p max] (a slider).
inline settings::setting_info int_setting(const std::string& category, const std::string& name,
                                          const std::string& label, const std::string& description,
                                          int32_t default_value, int32_t min, int32_t max, int32_t step = 1,
                                          const std::string& section = {}) {
  auto s = make_setting(category, name, settings::setting_type::integer, label, description, section);
  s.default_value = default_value;
  s.min = float(min);
  s.max = float(max);
  s.step = float(step);
  return s;
}

/// @brief One of @p options (a drop-down).
inline settings::setting_info choice_setting(const std::string& category, const std::string& name,
                                             const std::string& label, const std::string& description,
                                             const std::string& default_value,
                                             std::vector<settings::setting_option> options,
                                             const std::string& section = {}) {
  auto s = make_setting(category, name, settings::setting_type::choice, label, description, section);
  s.default_value = default_value;
  s.options = std::move(options);
  return s;
}

} // namespace bdg::wish::common
