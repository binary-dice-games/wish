// MIT License © 2026 Binary Dice Games
/// @file nano_settings.hpp
/// @brief nano's settings (see modules/bdg/common/tool_settings.hpp): the
///        Search panel's options, which the form starts with and saves when
///        they change.
#pragma once

#include "modules/bdg/common/tool_settings.hpp"

namespace bdg::wish::nano_settings {

inline constexpr const char* kCategory = "bdg.desktop.nano";
inline constexpr const char* kSearch = "Search";

inline settings::setting_info match_case() {
  return common::bool_setting(kCategory, "bdg.desktop.nano.match_case", "Match case",
                              "Searches tell upper and lower case apart.", false, kSearch);
}

inline settings::setting_info whole_word() {
  return common::bool_setting(kCategory, "bdg.desktop.nano.whole_word", "Match whole word only",
                              "Finds the text only as a whole word (normal and extended modes).", false, kSearch);
}

inline settings::setting_info search_mode() {
  return common::choice_setting(kCategory, "bdg.desktop.nano.search_mode", "Search mode",
                                "How the search text is read: as typed, with \\n, \\t... escapes, or as a "
                                "regular expression.",
                                "normal",
                                {{"normal", "Normal"}, {"extended", "Extended"}, {"regex", "Regular expression"}},
                                kSearch);
}

inline common::tool_settings all() {
  return {{kCategory, "nano", "Text editor.", "res/icons/edit.png", "#007AFF", 0},
          {match_case(), whole_word(), search_mode()}};
}

} // namespace bdg::wish::nano_settings
