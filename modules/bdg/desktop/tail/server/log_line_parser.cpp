// MIT License © 2025 Binary Dice Games
/// @file log_line_parser.cpp
/// @brief Implementation of log_line_parser.
#include "log_line_parser.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <unordered_set>

namespace bdg::wish {

namespace {

using njson = nlohmann::json;

std::string to_upper(const std::string& s) {
  std::string out = s;
  for (auto& c : out)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// Bracketed tokens that are themselves severity words (e.g. "[ERROR]") are
// never treated as a message tag -- see log_line_parser::parse()'s tag
// extraction below.
const std::unordered_set<std::string>& level_words() {
  static const std::unordered_set<std::string> kWords = {
      "TRACE", "VERBOSE", "DEBUG", "DBG", "INFO", "NOTICE", "WARN", "WARNING", "ERROR", "ERR",
      "CRITICAL", "CRIT", "SEVERE", "FATAL", "PANIC", "EMERGENCY", "EMERG", "EXCEPTION", "FAILURE", "FAILED"};
  return kWords;
}

std::string group_or_empty(const std::smatch& m, int group) {
  if (group <= 0)
    return {};
  auto idx = static_cast<size_t>(group);
  return idx < m.size() ? m[idx].str() : std::string{};
}

} // namespace

log_line_parser::log_line_parser() {
  load_builtin_defaults();
}

void log_line_parser::load_builtin_defaults() {
  // Minimal, always-available fallback used until (or if) patterns.json
  // fails to load: one bracket-level rule and a catch-all fallback, so
  // parse() still produces a reasonable result with no config file at all.
  line_formats_.clear();
  level_rules_.clear();

  log_line_format_rule bracket;
  bracket.name = "bracket_level";
  bracket.regex = std::regex(R"(^\[(TRACE|DEBUG|INFO|NOTICE|WARN(?:ING)?|ERROR|FATAL|CRITICAL)\]\s*(.*)$)",
      std::regex::icase);
  bracket.level_group = 1;
  bracket.message_group = 2;
  line_formats_.push_back(std::move(bracket));

  log_line_format_rule fallback;
  fallback.name = "fallback";
  fallback.regex = std::regex(R"(^(.*)$)");
  fallback.message_group = 1;
  line_formats_.push_back(std::move(fallback));

  level_rules_.push_back({"fatal", std::regex(R"(\b(FATAL|PANIC|EMERGENCY|EMERG)\b)", std::regex::icase), "#FF453AFF"});
  level_rules_.push_back(
      {"error", std::regex(R"(\b(ERROR|ERR|CRITICAL|CRIT|SEVERE)\b)", std::regex::icase), "#FF6961FF"});
  level_rules_.push_back({"warning", std::regex(R"(\b(WARN|WARNING)\b)", std::regex::icase), "#FFD60AFF"});
  level_rules_.push_back({"info", std::regex(R"(\b(INFO|NOTICE)\b)", std::regex::icase), "#64D2FFFF"});
  level_rules_.push_back({"debug", std::regex(R"(\b(DEBUG|DBG)\b)", std::regex::icase), "#B0B0B8FF"});
  level_rules_.push_back({"trace", std::regex(R"(\b(TRACE|VERBOSE)\b)", std::regex::icase), "#8E8E93FF"});

  default_color_ = "#D0D0D0FF";
  tag_regex_ = std::regex(R"(\[([A-Za-z0-9_][A-Za-z0-9_.:-]*)\])");
}

void log_line_parser::load_from_json(const std::string& json_text) {
  njson root;
  try {
    root = njson::parse(json_text);
  } catch (const njson::exception& e) {
    std::cerr << "[tail] failed to parse patterns.json: " << e.what() << " -- keeping previous rules\n";
    return;
  }

  std::vector<log_line_format_rule> line_formats;
  if (root.contains("line_formats") && root["line_formats"].is_array()) {
    for (auto& item : root["line_formats"]) {
      if (!item.is_object())
        continue;
      std::string pattern = item.value("regex", std::string{});
      if (pattern.empty())
        continue;
      log_line_format_rule rule;
      rule.name = item.value("name", std::string{});
      try {
        rule.regex = std::regex(pattern, std::regex::icase);
      } catch (const std::regex_error& e) {
        std::cerr << "[tail] skipping line_formats rule \"" << rule.name << "\": invalid regex: " << e.what()
                   << '\n';
        continue;
      }
      rule.timestamp_group = item.value("timestamp_group", -1);
      rule.level_group = item.value("level_group", -1);
      rule.message_group = item.value("message_group", -1);
      line_formats.push_back(std::move(rule));
    }
  }

  std::vector<log_level_rule> level_rules;
  if (root.contains("level_rules") && root["level_rules"].is_array()) {
    for (auto& item : root["level_rules"]) {
      if (!item.is_object())
        continue;
      std::string pattern = item.value("pattern", std::string{});
      if (pattern.empty())
        continue;
      log_level_rule rule;
      rule.level = item.value("level", std::string{"info"});
      try {
        rule.regex = std::regex(pattern, std::regex::icase);
      } catch (const std::regex_error& e) {
        std::cerr << "[tail] skipping level_rules rule \"" << rule.level << "\": invalid regex: " << e.what()
                   << '\n';
        continue;
      }
      rule.color = item.value("color", default_color_);
      level_rules.push_back(std::move(rule));
    }
  }

  if (line_formats.empty() || level_rules.empty()) {
    std::cerr << "[tail] patterns.json has no usable line_formats/level_rules -- keeping previous rules\n";
    return;
  }

  std::string default_color = root.value("default_level_color", default_color_);

  std::regex tag_regex = tag_regex_;
  if (root.contains("tag_pattern") && root["tag_pattern"].is_string()) {
    try {
      tag_regex = std::regex(root["tag_pattern"].get<std::string>());
    } catch (const std::regex_error& e) {
      std::cerr << "[tail] ignoring invalid tag_pattern: " << e.what() << '\n';
    }
  }

  line_formats_ = std::move(line_formats);
  level_rules_ = std::move(level_rules);
  default_color_ = default_color;
  tag_regex_ = std::move(tag_regex);
}

parsed_log_line log_line_parser::parse(const std::string& raw, const std::string& source) const {
  parsed_log_line pl;
  pl.raw = raw;
  pl.source = source;
  pl.message = raw;

  for (auto& fmt : line_formats_) {
    std::smatch m;
    if (!std::regex_match(raw, m, fmt.regex))
      continue;
    pl.timestamp = group_or_empty(m, fmt.timestamp_group);
    std::string level_text = group_or_empty(m, fmt.level_group);
    if (fmt.message_group > 0 && static_cast<size_t>(fmt.message_group) < m.size())
      pl.message = m[static_cast<size_t>(fmt.message_group)].str();

    std::string level_source = !level_text.empty() ? level_text : raw;
    for (auto& rule : level_rules_) {
      if (std::regex_search(level_source, rule.regex)) {
        pl.level = rule.level;
        pl.color = rule.color;
        break;
      }
    }
    break;
  }

  if (pl.color.empty())
    pl.color = default_color_;

  auto begin = std::sregex_iterator(raw.begin(), raw.end(), tag_regex_);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    if (it->size() < 2)
      continue;
    std::string candidate = (*it)[1].str();
    if (!level_words().count(to_upper(candidate))) {
      pl.tag = candidate;
      break;
    }
  }

  return pl;
}

} // namespace bdg::wish
