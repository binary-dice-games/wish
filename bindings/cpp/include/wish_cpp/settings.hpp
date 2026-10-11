// MIT License © 2026 Binary Dice Games
/**
 * @file settings.hpp
 * @brief Settings metadata for `client::describe_setting()` /
 *        `describe_settings_category()` (see docs/settings.md).
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bdg::wish::binding {

/// @brief A setting's default value: none, bool, int, float or text.
using setting_value = std::variant<std::monostate, bool, int32_t, double, std::string>;

/// @brief One choice of an enum setting.
struct setting_option {
  std::string value; ///< Stored value.
  std::string label; ///< Shown text; the value when empty.
};

/**
 * @brief Metadata of one setting: field `color` of the user store entry
 *        `bdg.desktop.example` is named `"bdg.desktop.example.color"`.
 */
struct setting_info {
  std::string name;
  std::string type{"string"}; ///< bool, int, float, string, enum, color or path.
  std::string label;
  std::string description;
  std::string category; ///< Sidebar category id.
  std::string section; ///< Group title inside the category.
  setting_value default_value;
  std::optional<double> min, max, step; ///< int and float settings.
  std::vector<setting_option> options; ///< enum settings.
  std::string icon;
  int32_t order{0};
  bool restart{false}; ///< A change takes effect only after a restart.
};

/// @brief Metadata of one sidebar category.
struct settings_category {
  std::string id;
  std::string label;
  std::string description;
  std::string icon;
  std::string color; ///< Badge color, "#RRGGBB".
  int32_t order{0};
};

namespace detail {

inline std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

inline std::string json_number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

inline std::string json_value(const setting_value& v) {
  if (std::holds_alternative<bool>(v))
    return std::get<bool>(v) ? "true" : "false";
  if (std::holds_alternative<int32_t>(v))
    return std::to_string(std::get<int32_t>(v));
  if (std::holds_alternative<double>(v))
    return json_number(std::get<double>(v));
  if (std::holds_alternative<std::string>(v))
    return json_string(std::get<std::string>(v));
  return "null";
}

} // namespace detail

/// @brief @p s as the JSON object `wish_settings_describe()` takes.
inline std::string to_json(const setting_info& s) {
  using detail::json_string;
  std::string j = "{\"name\":" + json_string(s.name) + ",\"type\":" + json_string(s.type) +
      ",\"label\":" + json_string(s.label) + ",\"description\":" + json_string(s.description) +
      ",\"category\":" + json_string(s.category) + ",\"section\":" + json_string(s.section) +
      ",\"default\":" + detail::json_value(s.default_value) + ",\"icon\":" + json_string(s.icon) +
      ",\"order\":" + std::to_string(s.order) + ",\"restart\":" + (s.restart ? "true" : "false");
  if (s.min)
    j += ",\"min\":" + detail::json_number(*s.min);
  if (s.max)
    j += ",\"max\":" + detail::json_number(*s.max);
  if (s.step)
    j += ",\"step\":" + detail::json_number(*s.step);
  if (!s.options.empty()) {
    j += ",\"options\":[";
    for (size_t i = 0; i < s.options.size(); ++i) {
      if (i)
        j += ",";
      j += "{\"value\":" + json_string(s.options[i].value) + ",\"label\":" + json_string(s.options[i].label) + "}";
    }
    j += "]";
  }
  return j + "}";
}

/// @brief @p c as the JSON object `wish_settings_describe_category()` takes.
inline std::string to_json(const settings_category& c) {
  using detail::json_string;
  return "{\"id\":" + json_string(c.id) + ",\"label\":" + json_string(c.label) +
      ",\"description\":" + json_string(c.description) + ",\"icon\":" + json_string(c.icon) +
      ",\"color\":" + json_string(c.color) + ",\"order\":" + std::to_string(c.order) + "}";
}

} // namespace bdg::wish::binding
