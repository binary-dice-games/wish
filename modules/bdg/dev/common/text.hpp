// MIT License © 2026 Binary Dice Games
/// @file text.hpp
/// @brief Small string helpers shared by the bdg/dev modules' clients for
///        handling command-line tool output and arguments.
///
/// Header-only and standard library only, so the modules' pure parsers (and
/// their unit tests, which link no bison/RMI/libuv) can use it too.
#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace bdg::wish::dev {

/// @brief @p s without leading / trailing spaces, tabs and line breaks.
inline std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  auto b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return {};
  auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

/// @brief @p s without its trailing line breaks (`\n` / `\r`).
inline std::string trim_eol(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
}

/// @brief Splits @p s on @p sep, keeping empty fields (`"a\t\tb"` -> 3
/// fields; `""` -> one empty field).
inline std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

/// @brief Splits @p s on whitespace runs, dropping empty fields -- for
/// space-padded columns and whitespace-separated lists.
inline std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string w;
  while (iss >> w)
    out.push_back(w);
  return out;
}

/// @brief Whether @p s begins with @p prefix.
inline bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

/// @brief @p text on one line, every whitespace run collapsed to a single
/// space, trailing space dropped, and cut to @p max characters (plus "...").
/// For a single-line preview of multi-line output or a multi-line argument.
inline std::string one_line(const std::string& text, size_t max) {
  std::string out;
  for (char ch : text) {
    const bool space = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
    if (!space)
      out += ch;
    else if (!out.empty() && out.back() != ' ')
      out += ' ';
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  if (out.size() > max)
    out = out.substr(0, max) + "...";
  return out;
}

/// @brief @p text from the first line that starts with one of @p markers
/// (e.g. `"Error:"`), dropping the warnings / progress chatter a tool prints
/// before its error message; @p text unchanged when no line does.
inline std::string from_marker_line(const std::string& text, std::initializer_list<const char*> markers) {
  size_t pos = std::string::npos;
  for (const char* marker : markers) {
    if (starts_with(text, marker))
      return text;
    const size_t at = text.find(std::string{"\n"} + marker);
    if (at != std::string::npos)
      pos = std::min(pos, at + 1);
  }
  return pos == std::string::npos ? text : text.substr(pos);
}

/// @brief Whether @p value may be passed as a positional argument: not empty
/// and not shaped like an option (`-n`, `--post-renderer=...`), which the
/// tool would otherwise parse as one.
inline bool is_safe_arg(const std::string& value) {
  return !value.empty() && value[0] != '-';
}

} // namespace bdg::wish::dev
