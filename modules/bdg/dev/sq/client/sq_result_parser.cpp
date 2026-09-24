// MIT License © 2026 Binary Dice Games
/// @file sq_result_parser.cpp
/// @brief Implementation of parse_jsonl_result() / mask_location().
#include "sq_result_parser.hpp"

#include <cctype>

namespace bdg::wish::sq {

namespace {

void append_utf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool parse_hex4(const std::string& s, size_t pos, unsigned& out) {
  if (pos + 4 > s.size())
    return false;
  out = 0;
  for (size_t i = 0; i < 4; ++i) {
    const char c = s[pos + i];
    out <<= 4;
    if (c >= '0' && c <= '9')
      out |= static_cast<unsigned>(c - '0');
    else if (c >= 'a' && c <= 'f')
      out |= static_cast<unsigned>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      out |= static_cast<unsigned>(c - 'A' + 10);
    else
      return false;
  }
  return true;
}

void skip_ws(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
    ++i;
}

// Parses the JSON string starting at s[i] == '"'; leaves i after the closing quote.
bool parse_string(const std::string& s, size_t& i, std::string& out) {
  if (i >= s.size() || s[i] != '"')
    return false;
  ++i;
  out.clear();
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"')
      return true;
    if (c != '\\') {
      out += c;
      continue;
    }
    if (i >= s.size())
      return false;
    const char e = s[i++];
    switch (e) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        unsigned cp = 0;
        if (!parse_hex4(s, i, cp))
          return false;
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
          unsigned lo = 0;
          if (parse_hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        append_utf8(out, cp);
        break;
      }
      default: out += e; break; // \" \\ \/ and anything unknown
    }
  }
  return false;
}

// Skips a nested object/array starting at s[i], respecting strings.
bool skip_nested(const std::string& s, size_t& i) {
  int depth = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      std::string ignored;
      if (!parse_string(s, i, ignored))
        return false;
      continue;
    }
    ++i;
    if (c == '{' || c == '[')
      ++depth;
    else if ((c == '}' || c == ']') && --depth == 0)
      return true;
  }
  return false;
}

bool parse_value(const std::string& s, size_t& i, sq_cell& cell) {
  skip_ws(s, i);
  if (i >= s.size())
    return false;
  cell = {};
  if (s[i] == '"')
    return parse_string(s, i, cell.text);
  if (s[i] == '{' || s[i] == '[') {
    const size_t start = i;
    if (!skip_nested(s, i))
      return false;
    cell.text = s.substr(start, i - start);
    return true;
  }
  const size_t start = i;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && !std::isspace(static_cast<unsigned char>(s[i])))
    ++i;
  cell.text = s.substr(start, i - start);
  if (cell.text.empty())
    return false;
  if (cell.text == "null") {
    cell.text.clear();
    cell.is_null = true;
  }
  return true;
}

// Parses one flat `{"k": v, ...}` line into keys + cells, in source order.
bool parse_object_line(const std::string& s, std::vector<std::string>& keys, std::vector<sq_cell>& cells) {
  size_t i = 0;
  skip_ws(s, i);
  if (i >= s.size() || s[i] != '{')
    return false;
  ++i;
  skip_ws(s, i);
  if (i < s.size() && s[i] == '}')
    return true;
  while (true) {
    skip_ws(s, i);
    std::string key;
    if (!parse_string(s, i, key))
      return false;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != ':')
      return false;
    ++i;
    sq_cell cell;
    if (!parse_value(s, i, cell))
      return false;
    keys.push_back(std::move(key));
    cells.push_back(std::move(cell));
    skip_ws(s, i);
    if (i >= s.size())
      return false;
    if (s[i] == '}')
      return true;
    if (s[i] != ',')
      return false;
    ++i;
  }
}

} // namespace

sq_result parse_jsonl_result(const std::string& text, size_t max_rows) {
  sq_result result;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos)
      nl = text.size();
    const std::string line = text.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.find_first_not_of(" \t\r") == std::string::npos)
      continue;

    std::vector<std::string> keys;
    std::vector<sq_cell> cells;
    if (!parse_object_line(line, keys, cells)) {
      ++result.malformed_lines;
      continue;
    }
    if (result.columns.empty())
      result.columns = keys;
    ++result.total_rows;
    if (result.rows.size() < max_rows)
      result.rows.push_back(std::move(cells));
  }
  result.truncated = result.total_rows > result.rows.size();
  return result;
}

std::string mask_location(const std::string& location) {
  const size_t scheme_end = location.find("://");
  if (scheme_end == std::string::npos)
    return location;
  const size_t auth_start = scheme_end + 3;
  const size_t at = location.find('@', auth_start);
  const size_t slash = location.find('/', auth_start);
  if (at == std::string::npos || (slash != std::string::npos && slash < at))
    return location;
  const size_t colon = location.find(':', auth_start);
  if (colon == std::string::npos || colon > at)
    return location;
  return location.substr(0, colon + 1) + "xxxxx" + location.substr(at);
}

} // namespace bdg::wish::sq
