// MIT License © 2025 Binary Dice Games
/// @file log_line_parser.hpp
/// @brief Parses raw log lines into timestamp/level/tag/message using a set
///        of configurable regex rules.
#pragma once

#include <regex>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief One line-shape rule: a regex tried against a whole raw line, plus
/// which capture group (1-based; -1 means "not present in this format")
/// holds the timestamp, severity word, and message body.
struct log_line_format_rule {
  std::string name;
  std::regex regex;
  int timestamp_group{-1};
  int level_group{-1};
  int message_group{-1};
};

/// @brief One severity classification rule: if `regex` matches (case
/// insensitive) the text being classified, the line is `level`, displayed
/// in `color` ("#RRGGBBAA", matches `Label.text_color`).
struct log_level_rule {
  std::string level;
  std::regex regex;
  std::string color;
};

/// @brief Result of parsing one raw log line.
struct parsed_log_line {
  std::string raw; ///< The original, unmodified line.
  std::string source; ///< Caller-supplied origin (e.g. file name); may be empty.
  std::string timestamp; ///< Extracted timestamp text, or empty if not found.
  std::string level; ///< Canonical lowercase level name ("error", "info", ...), or empty if unclassified.
  std::string color; ///< "#RRGGBBAA" text color to use for this line's Level/Message cells.
  std::string tag; ///< First non-severity `[Tag]` token found in the line, or empty.
  std::string message; ///< Extracted message body (falls back to `raw` if no rule captured one).
};

/// @brief Loads `log_line_format_rule`/`log_level_rule` sets from the
/// module's `patterns.json` config resource and classifies raw lines
/// against them.
///
/// Never throws: `load_from_json()` skips any rule with an invalid regex or
/// missing required field and logs the problem to stderr; if the whole
/// document fails to parse, the previously loaded rules (or the built-in
/// minimal fallback, if nothing loaded yet) are left in place.
class log_line_parser {
 public:
  log_line_parser();

  /// @brief Replace the current rule set by parsing @p json_text (the
  /// contents of patterns.json -- see that file for the schema and
  /// examples). On any parse/validation failure, logs to stderr and leaves
  /// the previously loaded rules untouched.
  void load_from_json(const std::string& json_text);

  /// @brief Classify one raw line: match it against `line_formats` (first
  /// match wins; the built-in fallback rule, `^(.*)$` as the whole
  /// message, always matches if the config's own rules didn't), extract a
  /// severity level and color, and look for a `[Tag]` token via
  /// `tag_pattern` (skipping any bracketed token that is itself a known
  /// severity word, e.g. `[ERROR]`).
  parsed_log_line parse(const std::string& raw, const std::string& source) const;

 private:
  void load_builtin_defaults();

  std::vector<log_line_format_rule> line_formats_;
  std::vector<log_level_rule> level_rules_;
  std::string default_color_{"#D0D0D0FF"};
  std::regex tag_regex_;
};

} // namespace bdg::wish
