// MIT License © 2026 Binary Dice Games
/// @file helm_table_parser.cpp
/// @brief Implementation of the helm table-output helpers.
#include "helm_table_parser.hpp"

#include <sstream>

namespace bdg::wish::helm {

using common::trim;

std::vector<std::vector<std::string>> parse_table(const std::string& text, const std::string& header, size_t ncols) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream iss(text);
  std::string line;
  bool first = true;
  while (std::getline(iss, line)) {
    if (line.find('\t') == std::string::npos)
      continue; // blank line or a plain message, not a table row.

    std::vector<std::string> cells = common::split(line, '\t');
    for (auto& cell : cells)
      cell = trim(cell);

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

std::string error_summary(const std::string& stderr_text) {
  return common::trim_eol(common::from_marker_line(stderr_text, {"Error:"}));
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
