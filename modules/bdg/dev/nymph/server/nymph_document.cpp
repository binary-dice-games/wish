// MIT License © 2026 Binary Dice Games
/// @file nymph_document.cpp
/// @brief Implementation of the nymph source splitter.
#include "nymph_document.hpp"

#include <algorithm>

namespace bdg::wish::nymph {

namespace {

std::string format_position(int line, int column, const std::string& message) {
  return std::to_string(line) + ":" + std::to_string(column) + ": " + message;
}

/// True if @p line (its line ending already removed) is a separator.
bool is_separator(std::string_view line) {
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
    line.remove_suffix(1);
  return line == "--";
}

int count_lines(std::string_view text) {
  return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
}

void end_with_newline(std::string& text) {
  if (!text.empty() && text.back() != '\n')
    text.push_back('\n');
}

} // namespace

error::error(int line, int column, const std::string& message)
    : std::runtime_error(format_position(line, column, message)), line_(line), column_(column), message_(message) {}

document parse_document(std::string_view text) {
  if (text.size() > kMaxSourceBytes)
    throw error(1, 1, "source is larger than 16 MiB");

  // Byte ranges [begin, end) of the first two separator lines, line ending
  // included.
  size_t sep_begin[2] = {0, 0};
  size_t sep_end[2] = {0, 0};
  int found = 0;
  size_t pos = 0;
  while (pos < text.size() && found < 2) {
    size_t eol = text.find('\n', pos);
    size_t next = eol == std::string_view::npos ? text.size() : eol + 1;
    size_t content_end = eol == std::string_view::npos ? text.size() : eol;
    if (is_separator(text.substr(pos, content_end - pos))) {
      sep_begin[found] = pos;
      sep_end[found] = next;
      ++found;
    }
    pos = next;
  }
  if (found < 2)
    throw error(1, 1,
        "expected three parts (description, format, data) separated by two '--' lines, found " +
            std::to_string(found) + " separator line" + (found == 1 ? "" : "s"));

  document doc;
  doc.description = std::string(text.substr(0, sep_begin[0]));
  doc.separator1 = std::string(text.substr(sep_begin[0], sep_end[0] - sep_begin[0]));
  doc.format = std::string(text.substr(sep_end[0], sep_begin[1] - sep_end[0]));
  doc.separator2 = std::string(text.substr(sep_begin[1], sep_end[1] - sep_begin[1]));
  doc.data = std::string(text.substr(sep_end[1]));
  doc.format_line = count_lines(text.substr(0, sep_end[0])) + 1;
  doc.data_line = count_lines(text.substr(0, sep_end[1])) + 1;
  return doc;
}

std::string compose_document(const document& doc) {
  std::string out;
  out.reserve(doc.description.size() + doc.separator1.size() + doc.format.size() + doc.separator2.size() +
              doc.data.size());
  out += doc.description;
  out += doc.separator1;
  out += doc.format;
  out += doc.separator2;
  out += doc.data;
  return out;
}

document make_document(std::string description, std::string format, std::string data) {
  end_with_newline(description);
  end_with_newline(format);
  document doc;
  doc.description = std::move(description);
  doc.format = std::move(format);
  doc.data = std::move(data);
  doc.format_line = count_lines(doc.description) + 2;
  doc.data_line = doc.format_line + count_lines(doc.format) + 1;
  return doc;
}

} // namespace bdg::wish::nymph
