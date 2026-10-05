// MIT License © 2026 Binary Dice Games
/// @file nymph_csv.hpp
/// @brief The data part of a nymph source: CSV text to a table of named
///        string columns, converted to numbers per column on demand.
///
/// Rules (DESIGN.md "CSV rules"): comma-separated, first row is the header,
/// RFC 4180 quoting, whitespace around an unquoted cell trimmed, blank
/// lines skipped, every row as wide as the header.
#pragma once

#include "nymph_document.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bdg::wish::nymph {

/// @brief Largest number of data rows nymph accepts.
inline constexpr size_t kMaxCsvRows = 1000000;

/// @brief A parsed CSV: column-major cells kept as text.
struct table {
  std::vector<std::string> headers;               ///< One per column.
  std::vector<std::vector<std::string>> columns;  ///< `columns[c][r]`, same size as `headers`.
  std::vector<int> row_lines;                     ///< Source line each data row starts on.

  /// @brief Number of data rows (the header is not one).
  size_t row_count() const { return row_lines.size(); }

  /// @brief Resolve a column reference: a header name, else a 1-based
  ///        position written in digits.
  /// @return The column index, or -1 if @p reference matches neither.
  int find_column(std::string_view reference) const;

  /// @brief Column @p column as numbers; an empty cell is NaN.
  /// @throws error (at the row's source line) for a cell that is neither
  ///         empty nor a number.
  std::vector<float> numbers(int column) const;
};

/// @brief Parse CSV text.
/// @param text        The data part of a source. Empty gives an empty table.
/// @param first_line  1-based source line of @p text's first line, so
///                    errors are positioned in the whole source.
/// @throws error for a ragged row, an unterminated quote, a duplicate or
///         empty header name, or more than `kMaxCsvRows` rows.
table parse_csv(std::string_view text, int first_line = 1);

} // namespace bdg::wish::nymph
