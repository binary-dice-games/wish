// MIT License © 2026 Binary Dice Games
/// @file nymph_csv.cpp
/// @brief Implementation of the nymph CSV parser.
#include "nymph_csv.hpp"

#include <algorithm>
#include <charconv>
#include <limits>

namespace bdg::wish::nymph {

namespace {

bool is_blank(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

std::string trimmed(std::string_view cell) {
  while (!cell.empty() && is_blank(cell.front()))
    cell.remove_prefix(1);
  while (!cell.empty() && is_blank(cell.back()))
    cell.remove_suffix(1);
  return std::string(cell);
}

/// One record (a header or data row) and where it started.
struct record {
  std::vector<std::string> cells;
  int line{1};
  bool blank{true}; ///< nothing but whitespace: skipped by the caller
};

/// Reads records one at a time, tracking the source line.
class reader {
 public:
  reader(std::string_view text, int first_line) : text_(text), line_(first_line) {
    if (text_.substr(0, 3) == "\xEF\xBB\xBF") // UTF-8 byte order mark
      pos_ = 3;
  }

  bool at_end() const { return pos_ >= text_.size(); }

  record next() {
    record rec;
    rec.line = line_;
    for (;;) {
      bool quoted = false;
      rec.cells.push_back(read_cell(quoted));
      if (quoted || !rec.cells.back().empty())
        rec.blank = false;
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        rec.blank = false;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '\n') {
        ++pos_;
        ++line_;
      }
      return rec;
    }
  }

 private:
  /// Reads up to (not past) the next unquoted ',' or '\n'.
  std::string read_cell(bool& quoted) {
    size_t start = pos_;
    size_t probe = pos_;
    while (probe < text_.size() && is_blank(text_[probe]))
      ++probe;
    if (probe < text_.size() && text_[probe] == '"') {
      quoted = true;
      return read_quoted(probe + 1);
    }
    while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '\n')
      ++pos_;
    return trimmed(text_.substr(start, pos_ - start));
  }

  std::string read_quoted(size_t from) {
    const int open_line = line_;
    std::string out;
    pos_ = from;
    for (;;) {
      if (pos_ >= text_.size())
        throw error(open_line, 1, "unterminated quoted CSV cell");
      char c = text_[pos_++];
      if (c == '"') {
        if (pos_ < text_.size() && text_[pos_] == '"') {
          out.push_back('"');
          ++pos_;
          continue;
        }
        break;
      }
      if (c == '\n')
        ++line_;
      out.push_back(c);
    }
    while (pos_ < text_.size() && is_blank(text_[pos_]))
      ++pos_;
    if (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '\n')
      throw error(line_, 1, "unexpected text after a quoted CSV cell");
    return out;
  }

  std::string_view text_;
  size_t pos_{0};
  int line_;
};

bool parse_number(const std::string& cell, float& out) {
  const char* first = cell.data();
  const char* last = first + cell.size();
  if (first != last && *first == '+')
    ++first;
  double value = 0.0;
  auto result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last)
    return false;
  out = static_cast<float>(value);
  return true;
}

} // namespace

int table::find_column(std::string_view reference) const {
  for (size_t i = 0; i < headers.size(); ++i)
    if (headers[i] == reference)
      return static_cast<int>(i);
  if (reference.empty() || !std::all_of(reference.begin(), reference.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return -1;
  size_t position = 0;
  auto result = std::from_chars(reference.data(), reference.data() + reference.size(), position);
  if (result.ec != std::errc{} || position < 1 || position > headers.size())
    return -1;
  return static_cast<int>(position - 1);
}

std::vector<float> table::numbers(int column) const {
  const auto& cells = columns.at(static_cast<size_t>(column));
  std::vector<float> out(cells.size());
  for (size_t r = 0; r < cells.size(); ++r) {
    if (cells[r].empty()) {
      out[r] = std::numeric_limits<float>::quiet_NaN();
      continue;
    }
    if (!parse_number(cells[r], out[r]))
      throw error(row_lines[r], 1,
          "column '" + headers[static_cast<size_t>(column)] + "': '" + cells[r] + "' is not a number");
  }
  return out;
}

table parse_csv(std::string_view text, int first_line) {
  table out;
  reader in(text, first_line);
  bool have_header = false;
  while (!in.at_end()) {
    record rec = in.next();
    if (rec.blank)
      continue;
    if (!have_header) {
      have_header = true;
      for (size_t i = 0; i < rec.cells.size(); ++i) {
        if (rec.cells[i].empty())
          throw error(rec.line, 1, "CSV header " + std::to_string(i + 1) + " is empty");
        if (std::find(out.headers.begin(), out.headers.end(), rec.cells[i]) != out.headers.end())
          throw error(rec.line, 1, "duplicate CSV header '" + rec.cells[i] + "'");
        out.headers.push_back(std::move(rec.cells[i]));
      }
      out.columns.resize(out.headers.size());
      continue;
    }
    if (rec.cells.size() != out.headers.size())
      throw error(rec.line, 1,
          "CSV row has " + std::to_string(rec.cells.size()) + " cells, the header has " +
              std::to_string(out.headers.size()));
    if (out.row_lines.size() >= kMaxCsvRows)
      throw error(rec.line, 1, "CSV has more than 1000000 data rows");
    for (size_t i = 0; i < rec.cells.size(); ++i)
      out.columns[i].push_back(std::move(rec.cells[i]));
    out.row_lines.push_back(rec.line);
  }
  return out;
}

} // namespace bdg::wish::nymph
