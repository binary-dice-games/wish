// MIT License © 2026 Binary Dice Games
/**
 * @file settings_store.hpp
 * @brief Reading and writing described settings in a `persistent_store`
 *        (server-side code; clients go through their user store API).
 *
 * See settings_schema.hpp for the metadata model and docs/settings.md.
 */
#pragma once

#include "context/persistent_store.hpp"
#include "context/settings_schema.hpp"

#include <string>
#include <vector>

namespace bdg::wish::settings {

/**
 * @brief Records @p info in @p store's `<entry>#schema` entry.
 * @throws schema_error for invalid metadata; std::runtime_error when the
 *         store can't be written.
 */
void describe(persistent_store& store, const setting_info& info);

/// @brief Records @p info in its own schema entry (category_entry_name()).
/// @throws schema_error for invalid metadata.
void describe_category(persistent_store& store, const category_info& info);

/// @brief A described setting and its current value.
struct described_setting {
  setting_info info;
  setting_value value; ///< The stored value; monostate when unset.
  /// The stored value, else the default.
  const setting_value& effective() const {
    return std::holds_alternative<std::monostate>(value) ? info.default_value : value;
  }
};

/// @brief Every category and described setting of a store.
struct catalog {
  std::vector<category_info> categories; ///< Merged by id (first one wins).
  std::vector<described_setting> settings; ///< Sorted by category, section, order, label.
};

/**
 * @brief Reads every schema entry of @p store and the matching values.
 * @param with_registered Also list the registered settings and categories
 *        (register_setting()), with their values in @p store; they win over
 *        a description of the same name stored in @p store. Pass it for a
 *        user store.
 */
catalog collect(const persistent_store& store, bool with_registered = false);

/**
 * @brief Validates @p value against @p info and writes it into its entry
 *        (read, modify, write; other fields are kept).
 * @return An error message, or empty on success.
 */
std::string write_setting(persistent_store& store, const setting_info& info, const setting_value& value);

/// @brief Removes the stored value of @p info, so its default applies.
void reset_setting(persistent_store& store, const setting_info& info);

/**
 * @brief The value of @p info in @p store: the stored one when it suits the
 *        setting (validate()), else the default. @p store may be null (an
 *        anonymous session): the default.
 */
setting_value value_of(const persistent_store* store, const setting_info& info);

/// @brief value_of() as a bool / int / float / string (the default's type
///        when the value has another).
bool bool_of(const persistent_store* store, const setting_info& info);
int32_t int_of(const persistent_store* store, const setting_info& info);
float float_of(const persistent_store* store, const setting_info& info);
std::string text_of(const persistent_store* store, const setting_info& info);

// ── Registry ─────────────────────────────────────────────────────────────────
//
// Settings that server-side code (e.g. a module's register_<name>()) knows
// about at startup. They live in every user store without being described
// there: settings tools list them for every identity, even before the app
// that uses them ever ran. Apps that aren't part of the server process
// describe their settings in the store instead (describe_setting()).

/// @brief Registers a user-store setting. Re-registering a name replaces it.
/// @throws schema_error for invalid metadata.
void register_setting(const setting_info& info);

/// @brief Registers a category. Re-registering an id replaces it.
/// @throws schema_error for invalid metadata.
void register_category(const category_info& info);

/// @brief Registered settings, in registration order.
std::vector<setting_info> registered_settings();

/// @brief Registered categories, in registration order.
std::vector<category_info> registered_categories();

} // namespace bdg::wish::settings
