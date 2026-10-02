// MIT License © 2026 Binary Dice Games
/// @file helm_table_parser.cpp
/// @brief Implementation of the helm table-output helpers.
#include "helm_table_parser.hpp"

#include <sstream>

namespace bdg::wish::helm {

namespace {

std::string trim(const std::string& s) {
  const char* ws = " \t\r\n";
  auto b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return {};
  auto e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

} // namespace

std::vector<std::vector<std::string>> parse_table(const std::string& text, const std::string& header, size_t ncols) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream iss(text);
  std::string line;
  bool first = true;
  while (std::getline(iss, line)) {
    if (line.find('\t') == std::string::npos)
      continue; // blank line or a plain message, not a table row.

    std::vector<std::string> cells;
    size_t start = 0;
    while (true) {
      size_t pos = line.find('\t', start);
      if (pos == std::string::npos) {
        cells.push_back(trim(line.substr(start)));
        break;
      }
      cells.push_back(trim(line.substr(start, pos - start)));
      start = pos + 1;
    }

    const bool is_header = first && cells[0] == header;
    first = false;
    if (is_header)
      continue;
    cells.resize(ncols);
    rows.push_back(std::move(cells));
  }
  return rows;
}

std::string short_timestamp(const std::string& updated) {
  if (updated.size() >= 19 && updated[4] == '-' && updated[7] == '-' && updated[10] == ' ' && updated[13] == ':' &&
      updated[16] == ':')
    return updated.substr(0, 19);
  return updated;
}

bool is_safe_arg(const std::string& value) {
  return !value.empty() && value[0] != '-';
}

std::string error_summary(const std::string& stderr_text) {
  std::string text = stderr_text;
  // An "Error:" at the very start or at the start of a later line.
  size_t pos = text.rfind("Error:", 0) == 0 ? 0 : text.find("\nError:");
  if (pos != std::string::npos && pos != 0)
    text = text.substr(pos + 1);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

bool is_valid_release_name(const std::string& name) {
  if (name.empty() || name.size() > 53 || name.front() == '-' || name.back() == '-')
    return false;
  for (char ch : name) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
      return false;
  }
  return true;
}

} // namespace bdg::wish::helm
