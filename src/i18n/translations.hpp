// MIT License © 2025 Binary Dice Games
/// @file translations.hpp
/// @brief Translation maps for internationalized (i18n) UI templates.
///
/// A template string whose *whole value* starts with `$$` is translatable:
/// the rest of the string is a key looked up in a `translation_map` (e.g.
/// `"$$TAIL_FOLLOW"` -> key `TAIL_FOLLOW`). There is no token scanning inside
/// longer strings, so detection is a two-character prefix test and
/// substitution replaces the whole value.
///
/// Translation files hold one `KEY = value` pair per line (see
/// `parse_translations()`). A map can chain to a fallback map (usually the
/// `en` file), and a key found in neither shows up as the bare key so the gap
/// is visible in the UI.
///
/// Client-safe: no dependency on the server-side UI element registry.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Prefix that marks a template string as a translation key.
inline constexpr std::string_view translation_prefix = "$$";

/// @brief Key -> translated text map with an optional fallback chain.
class translation_map {
 public:
  translation_map() = default;

  /// @brief Add or replace one entry.
  void set(std::string key, std::string value) {
    entries_[std::move(key)] = std::move(value);
  }

  /// @brief Map consulted when a key is missing here (may be null).
  void set_fallback(std::shared_ptr<const translation_map> fallback) {
    fallback_ = std::move(fallback);
  }
  const std::shared_ptr<const translation_map>& fallback() const {
    return fallback_;
  }

  /// @brief Look @p key up here, then along the fallback chain.
  /// @return A pointer to the translated text, or null if no map has it.
  const std::string* find(std::string_view key) const;

  /// @brief Like `find()`, but returns @p key itself when it is missing.
  std::string lookup(std::string_view key) const;

  /// @brief True when neither this map nor its fallback chain has entries.
  bool empty() const {
    return entries_.empty() && (!fallback_ || fallback_->empty());
  }

  /// @brief Copy @p other's own entries (not its fallbacks) into this map;
  ///        entries already here win.
  void merge(const translation_map& other) {
    entries_.insert(other.entries_.begin(), other.entries_.end());
  }

  /// @brief Number of entries in this map (fallbacks not counted).
  size_t size() const {
    return entries_.size();
  }

 private:
  std::unordered_map<std::string, std::string> entries_;
  std::shared_ptr<const translation_map> fallback_;
};

/// @brief Parse translation file text.
///
/// Format (UTF-8): one `KEY = value` per line. Key and value are trimmed of
/// surrounding spaces/tabs. Blank lines and lines whose first non-blank
/// character is `#` or `;` are skipped. In values, `\n`, `\t` and `\\` are
/// unescaped. A later duplicate key wins. Lines without `=` or with an empty
/// key are ignored. CRLF line endings are accepted.
translation_map parse_translations(std::string_view text);

/// @brief Read and parse a translation file.
/// @return The parsed map, or `std::nullopt` if the file cannot be read.
std::optional<translation_map> load_translations_file(const std::filesystem::path& path);

/// @brief True when @p lang is a usable language code: letters, digits, `-`
///        and `_` only (empty is valid and means `en`). Codes become file
///        names, so anything else is rejected.
bool is_valid_language_code(std::string_view lang);

/// @brief True when @p text is a translation key reference (starts with `$$`).
inline bool is_translatable(std::string_view text) {
  return text.substr(0, translation_prefix.size()) == translation_prefix;
}

/// @brief Separates a message's key from its arguments (ASCII unit
///        separator); see make_message().
inline constexpr char message_arg_separator = '\x1f';

/// @brief Translate one string.
///
/// A message built by make_message() (`"$$key\x1farg0\x1farg1..."`) is
/// translated as `format_text(map.lookup(key), {arg0, arg1, ...})`.
///
/// @return `map.lookup(key)` when @p text is `"$$key"`; @p text unchanged
///         otherwise.
std::string translate_text(std::string_view text, const translation_map& map);

/// @brief Encode a translation key and its `{0}`, `{1}`, ... arguments as
///        one string, translated wherever translate_text() runs.
///
/// Lets code that has no translations of its own -- e.g. a client-side
/// module reporting progress to its server-side form -- send a message the
/// receiving side translates: `make_message("$$DU_SCANNED", {"5", "3 MB"})`.
/// Arguments must not contain `message_arg_separator`.
std::string make_message(std::string_view key, const std::vector<std::string>& args);

/// @brief Replace `{0}`, `{1}`, ... in @p pattern with @p args.
///
/// For translated messages with values in them, so each language can put
/// the values where its grammar needs them (`"{0} lines"` / `"{0} líneas"`).
/// Placeholders without a matching argument are left as they are.
std::string format_text(std::string_view pattern, const std::vector<std::string>& args);

} // namespace bdg::wish
