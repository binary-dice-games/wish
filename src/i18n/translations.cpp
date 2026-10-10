// MIT License © 2025 Binary Dice Games
/// @file translations.cpp
/// @brief Translation file parsing and `$$KEY` string lookup.
#include <i18n/translations.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace bdg::wish {

namespace {

std::string_view trim(std::string_view s) {
  size_t b = s.find_first_not_of(" \t");
  if (b == std::string_view::npos)
    return {};
  size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

std::string unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char next = s[i + 1];
      if (next == 'n' || next == 't' || next == '\\') {
        out += next == 'n' ? '\n' : next == 't' ? '\t' : '\\';
        ++i;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

} // namespace

// ── translation_map ─────────────────────────────────────────────────────────

const std::string* translation_map::find(std::string_view key) const {
  for (const translation_map* m = this; m; m = m->fallback_.get()) {
    // unordered_map<std::string> has no heterogeneous lookup before C++20's
    // transparent hashing, so build the key once per map level.
    auto it = m->entries_.find(std::string{key});
    if (it != m->entries_.end())
      return &it->second;
  }
  return nullptr;
}

std::string translation_map::lookup(std::string_view key) const {
  const std::string* value = find(key);
  return value ? *value : std::string{key};
}

// ── Parsing ─────────────────────────────────────────────────────────────────

translation_map parse_translations(std::string_view text) {
  translation_map map;
  while (!text.empty()) {
    size_t eol = text.find('\n');
    std::string_view line = text.substr(0, eol);
    text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);

    line = trim(line);
    if (line.empty() || line.front() == '#' || line.front() == ';')
      continue;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos)
      continue;
    std::string_view key = trim(line.substr(0, eq));
    if (key.empty())
      continue;
    map.set(std::string{key}, unescape(trim(line.substr(eq + 1))));
  }
  return map;
}

std::optional<translation_map> load_translations_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_translations(ss.str());
}

bool is_valid_language_code(std::string_view lang) {
  for (unsigned char c : lang) {
    if (!std::isalnum(c) && c != '-' && c != '_')
      return false;
  }
  return true;
}

// ── Lookup ──────────────────────────────────────────────────────────────────

std::string translate_text(std::string_view text, const translation_map& map) {
  if (!is_translatable(text))
    return std::string{text};
  return map.lookup(text.substr(translation_prefix.size()));
}

} // namespace bdg::wish
