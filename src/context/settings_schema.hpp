// MIT License © 2026 Binary Dice Games
/**
 * @file settings_schema.hpp
 * @brief Metadata that describes the settings apps keep in the persistent
 *        stores, so tools like `bdg/desktop/conf` can build an editor for
 *        them.
 *
 * A setting is one field of a store entry: `bdg.desktop.example.color` is
 * the field `color` of the entry `bdg.desktop.example`. Its metadata lives
 * in the same store as its value, in the reserved entry
 * `bdg.desktop.example#schema`, so a settings tool can show the settings of
 * an app that isn't running, and a client can only describe settings of its
 * own user store. See docs/settings.md.
 *
 * Schema entry layout (index-keyed lists, because the store keeps field
 * names only as hashes):
 *
 * ```
 * { settings:   [ {name, label, description, category, section, type,
 *                  default, min, max, step, options: [{value, label}],
 *                  icon, order, restart}, ... ],
 *   categories: [ {id, label, description, icon, color, order}, ... ] }
 * ```
 *
 * Everything here is pure (no I/O); see settings_store.hpp for the
 * functions that read and write a `persistent_store`.
 */
#pragma once

#include "src/bison/bison_object.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bdg::wish::settings {

/// @brief Suffix of the reserved entry holding an entry's settings metadata.
inline constexpr const char* kSchemaSuffix = "#schema";

/// @brief A setting's value: none, bool, int, float or text (string, enum,
///        color and path settings).
using setting_value = std::variant<std::monostate, bool, int32_t, float, std::string>;

/// @brief Value types a setting can have.
enum class setting_type {
  boolean, ///< "bool": a switch.
  integer, ///< "int": a slider when min and max are set, else a number box.
  real, ///< "float": same as int, with decimals.
  text, ///< "string": a text box.
  choice, ///< "enum": one of @ref setting_info::options.
  color, ///< "color": "#RRGGBB" or "#RRGGBBAA".
  path, ///< "path": a file or directory path, as text.
};

/// @brief The type's name in metadata ("bool", "int", ...).
const char* type_name(setting_type type);

/// @brief Parses a type name; nullopt if unknown.
std::optional<setting_type> parse_type(const std::string& name);

/// @brief One choice of an enum setting.
struct setting_option {
  std::string value; ///< Stored value.
  std::string label; ///< Shown text; the value when empty.
};

/// @brief Metadata of one setting.
struct setting_info {
  /// Dotted name: the entry, a dot, then the field (e.g.
  /// "bdg.desktop.example.color").
  std::string name;
  std::string label; ///< Shown name; the field when empty.
  std::string description; ///< One or two sentences shown under the label.
  std::string category; ///< Sidebar category id (e.g. "appearance").
  std::string section; ///< Group title inside the category; may be empty.
  setting_type type{setting_type::text};
  setting_value default_value; ///< Shown when the store has no value yet.
  std::optional<float> min, max, step; ///< int and float settings.
  std::vector<setting_option> options; ///< enum settings.
  std::string icon; ///< Optional row icon, e.g. "res/icons/palette.png".
  int32_t order{0}; ///< Sort key inside its section (then the label).
  bool restart{false}; ///< True when a change takes effect only after a restart.
};

/// @brief Metadata of one sidebar category.
struct category_info {
  std::string id; ///< Referenced by @ref setting_info::category.
  std::string label; ///< Shown name; the id when empty.
  std::string description; ///< Shown in the category's header.
  std::string icon; ///< e.g. "res/icons/palette.png".
  std::string color; ///< Badge color, "#RRGGBB"; empty for gray.
  int32_t order{0}; ///< Sort key in the sidebar (then the label).
};

/// @brief Thrown for invalid metadata (a bad name, type or option list).
class schema_error : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// @brief A setting's store location.
struct setting_location {
  std::string entry; ///< Store entry, e.g. "bdg.desktop.example".
  std::string field; ///< Field of that entry, e.g. "color".
};

/**
 * @brief Splits @p name at its last dot.
 * @throws schema_error unless the entry is a valid store name without '#'
 *         and the field is made of letters, digits, '_' and '-'.
 */
setting_location parse_name(const std::string& name);

/// @brief Schema entry holding category @p id's metadata
///        ("wish.category.<id>#schema"); categories get their own entry so
///        any app can declare or extend one.
std::string category_entry_name(const std::string& id);

/// @brief Name of the entry holding @p entry's metadata ("<entry>#schema").
std::string schema_entry_name(const std::string& entry);

/// @brief Whether @p name is a schema entry; sets @p entry to the entry it
///        describes when it is.
bool is_schema_entry(const std::string& name, std::string* entry = nullptr);

/**
 * @brief Checks @p info: its name, its default value's type, its range and
 *        its options.
 * @throws schema_error describing the first problem.
 */
void check(const setting_info& info);

/// @brief Checks @p info's id and color. @throws schema_error.
void check(const category_info& info);

/**
 * @brief Adds @p info to the schema entry @p schema, replacing the record
 *        with the same name. @p info must pass check().
 */
void merge_setting(bison::dynamic& schema, const setting_info& info);

/// @brief Adds @p info to @p schema, replacing the category with the same id.
void merge_category(bison::dynamic& schema, const category_info& info);

/// @brief Settings recorded in a schema entry (invalid records skipped).
std::vector<setting_info> settings_of(const bison::dynamic& schema);

/// @brief Categories recorded in a schema entry (invalid records skipped).
std::vector<category_info> categories_of(const bison::dynamic& schema);

/// @brief Setting metadata as a JSON object (the C ABI's format).
std::string to_json(const setting_info& info);

/// @brief Category metadata as a JSON object.
std::string to_json(const category_info& info);

/**
 * @brief Parses setting metadata from JSON, e.g.
 *        `{"name": "a.b.color", "type": "color", "default": "#FF0000"}`.
 * @throws schema_error for malformed JSON or invalid metadata.
 */
setting_info setting_from_json(const std::string& json);

/// @brief Parses category metadata from JSON. @throws schema_error.
category_info category_from_json(const std::string& json);

/**
 * @brief Checks that @p value suits @p info (type, range, enum option,
 *        color format) after coercing it to the setting's type (e.g. an int
 *        for a float setting).
 * @return The coerced value, or an error message.
 */
std::variant<setting_value, std::string> validate(const setting_info& info, const setting_value& value);

/// @brief Reads field @p field of @p entry as @p type; monostate when the
///        field is missing or can't be converted.
setting_value read_value(const bison::dynamic& entry, const std::string& field, setting_type type);

/// @brief Writes @p value into field @p field of @p entry (erasing it for
///        monostate).
void write_value(bison::dynamic& entry, const std::string& field, const setting_value& value);

/// @brief The value as display text ("true", "12", "#FF0000", ...).
std::string to_text(const setting_value& value);

} // namespace bdg::wish::settings
