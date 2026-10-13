// MIT License © 2026 Binary Dice Games
/// @file find_settings.hpp
/// @brief find's settings (see modules/bdg/common/tool_settings.hpp), shared
///        by its server form and its client runner.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::find_settings {

inline constexpr const char* kCategory = "bdg.desktop.find";

// ── Database (client) ───────────────────────────────────────────────────────

/// @brief Folders indexed when find is run without folder arguments.
inline settings::setting_info locations() {
  auto s = common::make_setting(
      kCategory,
      "bdg.desktop.find.locations",
      settings::setting_type::text,
      "Indexed folders",
      "Folders searched, separated by \";\". Empty indexes your home folder. Applies the next "
      "time the database is updated.",
      "Database");
  s.default_value = std::string{};
  return s;
}

/// @brief Names or absolute paths left out of the database.
inline settings::setting_info excludes() {
  auto s = common::make_setting(
      kCategory,
      "bdg.desktop.find.excludes",
      settings::setting_type::text,
      "Excluded folders",
      "Folder names (such as node_modules) or absolute paths left out of the database, "
      "separated by \";\".",
      "Database");
  s.default_value = std::string{};
  return s;
}

inline settings::setting_info exclude_hidden() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.exclude_hidden",
      "Exclude hidden files and folders",
      "Leaves names starting with a dot out of the database.",
      false,
      "Database");
}

inline settings::setting_info one_filesystem() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.one_filesystem",
      "Stay on one filesystem",
      "Does not walk into other disks or mounts (such as /proc) below an indexed folder.",
      true,
      "Database");
}

// ── Search (server form; the Search menu saves them) ────────────────────────

inline settings::setting_info match_case() {
  return common::bool_setting(
      kCategory, "bdg.desktop.find.match_case", "Match case", "The Search menu's Match Case.", false, "Search");
}

inline settings::setting_info search_in_path() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.search_in_path",
      "Search in path",
      "The Search menu's Search in Path: match the full path, not only the name.",
      false,
      "Search");
}

inline settings::setting_info regex() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.regex",
      "Regular expressions",
      "The Search menu's Regular Expression: the query is one regular expression.",
      false,
      "Search");
}

inline settings::setting_info auto_match_case() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.auto_match_case",
      "Match case for upper-case queries",
      "Matches case automatically when the query has an upper-case letter.",
      true,
      "Search");
}

inline settings::setting_info auto_search_in_path() {
  return common::bool_setting(
      kCategory,
      "bdg.desktop.find.auto_search_in_path",
      "Search in path for path queries",
      "Searches a term in the full path automatically when it contains a path separator.",
      true,
      "Search");
}

// ── View (server form; the View menu saves them) ────────────────────────────

inline settings::setting_info show_statusbar() {
  return common::bool_setting(
      kCategory, "bdg.desktop.find.show_statusbar", "Status bar", "The View menu's Status Bar.", true, "View");
}

inline settings::setting_info show_filter() {
  return common::bool_setting(
      kCategory, "bdg.desktop.find.show_filter", "Filter", "The View menu's Filter drop-down.", true, "View");
}

inline common::tool_settings all() {
  return {
      {kCategory, "find", "File search.", "res/icons/search.png", "#007AFF", 0},
      {locations(), excludes(), exclude_hidden(), one_filesystem(), match_case(), search_in_path(), regex(),
       auto_match_case(), auto_search_in_path(), show_statusbar(), show_filter()}};
}

} // namespace bdg::wish::find_settings
