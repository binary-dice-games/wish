// MIT License © 2026 Binary Dice Games
/// @file sq_result_parser.hpp
/// @brief Dependency-free parsing helpers for `sq` output: the LF-delimited
///        JSON objects (`--jsonl`) query-result format, and connection
///        location masking.
///
/// Split out of sq_source so it is unit-testable without bison/RMI/libuv.
/// `--jsonl` is used (rather than `--json` parsed into a bison `dynamic`)
/// because a `dynamic` loses the column *order* (keys are hashed) and would
/// merge same-named columns, and (rather than `--csv`) because JSON keeps
/// SQL NULL distinct from an empty string.
#pragma once

#include <string>
#include <vector>

namespace bdg::wish::sq {

/// @brief One result cell: the display text, or SQL NULL.
struct sq_cell {
  std::string text;
  bool is_null{false};
};

/// @brief A parsed query result.
struct sq_result {
  std::vector<std::string> columns;        ///< From the first row's keys, in order.
  std::vector<std::vector<sq_cell>> rows;  ///< At most `max_rows` rows.
  size_t total_rows{0};                    ///< Every well-formed row seen, incl. dropped.
  bool truncated{false};                   ///< `total_rows > rows.size()`.
  size_t malformed_lines{0};               ///< Non-empty lines that were not a JSON object.
};

/// @brief Parses `sq ... --jsonl` output (one flat JSON object per line).
///
/// Strings are unescaped (including `\uXXXX` surrogate pairs, to UTF-8);
/// numbers and booleans keep their literal text; `null` becomes a NULL cell;
/// a nested object/array value keeps its raw JSON text. A zero-row result has
/// no columns (`--jsonl` prints nothing for it). Only the first @p max_rows
/// rows are materialized; the rest are counted in `total_rows`.
sq_result parse_jsonl_result(const std::string& text, size_t max_rows);

/// @brief Replaces the password in a `scheme://user:password@host/...`
/// location with `xxxxx`; any other location is returned unchanged.
std::string mask_location(const std::string& location);

} // namespace bdg::wish::sq
