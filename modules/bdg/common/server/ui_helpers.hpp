// MIT License © 2026 Binary Dice Games
/// @file ui_helpers.hpp
/// @brief Small helpers shared by the bdg modules' server-side forms: theme
///        colours, payload readers, child-list building.
///
/// Server-side counterpart of `modules/bdg/common/text.hpp`. Lives in
/// `modules/bdg/common/server/`, which wish_add_module() compiles into
/// `wish_server` for every enabled bdg module that has a `server/`
/// directory (see cmake/WishModules.cmake). Header-only.
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish::common {

using bison::operator""_key;

// ── Theme colours ─────────────────────────────────────────────────────────

/// @brief A `"#RRGGBBAA"` text colour pair: one tuned for the light theme,
/// one for the dark theme (a single colour reads poorly on the other).
struct theme_color {
  const char* light;
  const char* dark;
};

// GitHub Primer tokens.
inline constexpr theme_color kOk{"#1A7F37FF", "#3FB950FF"}; ///< success / running
inline constexpr theme_color kIdle{"#656D76FF", "#8B949EFF"}; ///< secondary text
inline constexpr theme_color kWarn{"#9A6700FF", "#D29922FF"}; ///< pending / attention
inline constexpr theme_color kBad{"#CF222EFF", "#F85149FF"}; ///< failure

/// @brief Sets @p el's `text_color_light` / `text_color_dark` to @p color.
inline void set_text_color(const ui_element_ptr& el, const theme_color& color) {
  el["text_color_light"_key] = std::string{color.light};
  el["text_color_dark"_key] = std::string{color.dark};
}

/// @brief Sets status label @p label to @p text, idle-grey when @p ok and red
/// otherwise. No-op when @p label is null.
inline void set_status_text(const ui_element_ptr& label, const std::string& text, bool ok) {
  if (!label)
    return;
  label["text"_key] = text;
  set_text_color(label, ok ? kIdle : kBad);
}

// ── Icons ─────────────────────────────────────────────────────────────────

/// @brief Session path of built-in icon @p name (`resources/embedded/icons/
/// <name>.png`, extracted into every session): `"res/icons/<name>.png"`.
/// Use it for a Button / MenuButton / MenuItem / TreeNode `icon` field.
inline std::string icon_path(const std::string& name) {
  return "res/icons/" + name + ".png";
}

/// @brief Built-in icon name for a common action label ("Refresh",
/// "Remove", "Logs", ...), or `""` when none fits. Matches the label's first
/// word, case-insensitively, ignoring a trailing "..." -- so "Rollback to
/// this revision" and "Prune stopped..." match too. Used by table_rows for
/// row menu items, so every tool's row actions get the same icons.
inline std::string action_icon(const std::string& label) {
  std::string verb;
  for (char c : label) {
    if (c == ' ' || c == '.')
      break;
    verb += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  static const std::pair<const char*, const char*> kIcons[] = {
      {"refresh", "refresh"},
      {"rescan", "refresh"},
      {"reload", "refresh"},
      {"restart", "refresh"},
      {"reinstall", "refresh"},
      {"update", "sync"},
      {"fetch", "sync"},
      {"sync", "sync"},
      {"remove", "delete"},
      {"delete", "delete"},
      {"uninstall", "delete"},
      {"prune", "delete"},
      {"clear", "delete"},
      {"drop", "delete"},
      {"kill", "close"},
      {"inspect", "info"},
      {"describe", "info"},
      {"details", "info"},
      {"status", "info"},
      {"properties", "info"},
      {"logs", "document"},
      {"notes", "document"},
      {"readme", "document"},
      {"values", "document"},
      {"manifest", "code"},
      {"files", "folder_open"},
      {"open", "folder_open"},
      {"load", "folder_open"},
      {"start", "play"},
      {"run", "play"},
      {"unpause", "play"},
      {"resume", "play"},
      {"stop", "stop"},
      {"pause", "pause"},
      {"install", "download"},
      {"pull", "download"},
      {"download", "download"},
      {"upgrade", "arrow_up"},
      {"pop", "arrow_up"},
      {"push", "upload"},
      {"upload", "upload"},
      {"rollback", "undo"},
      {"history", "history"},
      {"versions", "tag"},
      {"tag", "tag"},
      {"save", "save"},
      {"bookmark", "bookmark"},
      {"new", "add"},
      {"create", "add"},
      {"add", "add"},
      {"edit", "edit"},
      {"rename", "edit"},
      {"copy", "copy"},
      {"duplicate", "copy"},
      {"paste", "paste"},
      {"cut", "cut"},
      {"search", "search"},
      {"find", "search"},
      {"filter", "filter"},
      {"lock", "lock"},
      {"cordon", "lock"},
      {"unlock", "lock_open"},
      {"uncordon", "lock_open"},
      {"drain", "logout"},
      {"show", "visibility"},
      {"hide", "visibility_off"},
      {"expand", "expand_more"},
      {"collapse", "expand_less"},
      {"back", "arrow_back"},
      {"forward", "arrow_forward"},
      {"fullscreen", "fullscreen"},
      {"login", "login"},
      {"logout", "logout"},
      {"share", "share"},
      {"print", "print"},
      {"mail", "mail"},
      {"email", "mail"},
      {"schedule", "schedule"},
      {"notifications", "notifications"},
      {"chart", "chart"},
      {"plot", "chart"},
      {"theme", "palette"},
      {"settings", "settings"},
      {"preferences", "settings"},
      {"configure", "tune"},
      {"help", "help"},
      {"checkout", "check"},
      {"apply", "check"},
      {"connect", "link"},
      {"ping", "server"},
      {"merge", "merge"},
      {"commit", "commit"},
  };
  for (const auto& [word, icon] : kIcons)
    if (verb == word)
      return icon;
  return {};
}

/// @brief Sets @p el's `icon` to built-in icon @p name (see icon_path()).
template <typename Element>
void set_icon(const Element& el, const std::string& name) {
  el["icon"_key] = icon_path(name);
}

// ── Element helpers ───────────────────────────────────────────────────────

/// @brief The `__wish_id` an element was registered under.
template <typename Element>
bison::key_t wish_id_of(const Element& element) {
  return element->template as<bison::key_t>("__wish_id"_key);
}

/// @brief Replaces @p parent's `children` with @p kids, in order.
inline void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids) {
  auto list = bison::dynamic_ptr{bison::key_t{0U}, {}};
  size_t k = 0;
  for (auto& kid : kids)
    (*list)[k++] = bison::dynamic_ptr{kid};
  (*parent)["children"_key] = list;
  parent->refresh_children_order();
}

// ── Payload readers ───────────────────────────────────────────────────────

/// @brief Calls @p fn with every object entry of the array field
/// @p field_key of @p parent (the `{ <key>: [ {...}, ... ] }` shape every
/// `update_*` RMI method takes). No-op when the field is missing.
template <typename Fn>
void for_each_entry(const bison::dynamic& parent, bison::key_t field_key, Fn&& fn) {
  const auto* arr_f = parent.findField<bison::dynamic_ptr>(field_key);
  if (!arr_f || !*arr_f)
    return;
  (*arr_f)->forEach([&](bison::key_t, const bison::field& f) {
    if (!f.is<bison::dynamic_ptr>())
      return;
    auto entry_ptr = f.as<bison::dynamic_ptr>();
    if (entry_ptr)
      fn(*entry_ptr);
  });
}

namespace payload_detail {
inline void fill_payload(bison::dynamic&) {}
template <typename T, typename... Rest>
void fill_payload(bison::dynamic& d, bison::key_t k, T&& v, Rest&&... rest) {
  d[k] = std::forward<T>(v);
  fill_payload(d, std::forward<Rest>(rest)...);
}
} // namespace payload_detail

/// @brief An event payload from key / value pairs:
/// `make_payload("id"_key, id, "action"_key, action)` (bison::dynamic has
/// no initializer-list constructor). Pass string literals as std::string.
template <typename... Args>
bison::dynamic make_payload(Args&&... args) {
  bison::dynamic d;
  payload_detail::fill_payload(d, std::forward<Args>(args)...);
  return d;
}

/// @brief String field @p k of @p d; `""` when absent or not a string.
inline std::string str_of(const bison::dynamic& d, bison::key_t k) {
  const auto* f = d.findField<std::string>(k);
  return f ? *f : std::string{};
}

/// @brief Boolean field @p k of @p d; false when absent or not a bool.
inline bool flag_of(const bison::dynamic& d, bison::key_t k) {
  const auto* f = d.findField<bool>(k);
  return f && *f;
}

/// @brief @p s lower-cased (ASCII) -- for case-insensitive filters.
inline std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
  return s;
}

/// @brief `"1 package"` / `"3 packages"`.
inline std::string plural(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

/// @brief `"<command>: OK"` or `"<command> failed: <output>"` (`"unknown
/// error"` when the output is empty) -- the status-label text a form shows
/// for a `command_result {command, ok, output}` call.
/// @param[out] ok  The call's `ok` field.
inline std::string command_result_text(const bison::dynamic& args, bool& ok) {
  ok = flag_of(args, "ok"_key);
  const std::string command = str_of(args, "command"_key);
  if (ok)
    return command + ": OK";
  const std::string output = str_of(args, "output"_key);
  return command + " failed: " + (output.empty() ? std::string{"unknown error"} : output);
}

} // namespace bdg::wish::common
