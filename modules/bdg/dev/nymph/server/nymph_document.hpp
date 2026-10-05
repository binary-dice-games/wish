// MIT License © 2026 Binary Dice Games
/// @file nymph_document.hpp
/// @brief A nymph source text split into its three parts (description,
///        format YAML, data CSV), and the error type every nymph parser
///        throws. No YAML or CSV knowledge; see DESIGN.md "3. Source Format".
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bdg::wish::nymph {

/// @brief Largest source text nymph accepts, in bytes.
inline constexpr size_t kMaxSourceBytes = 16u * 1024u * 1024u;

/// @brief A problem in a source text, positioned in the *whole* source.
///
/// `what()` is `"<line>:<column>: <message>"`, both 1-based, ready to be
/// prefixed with a file name.
class error : public std::runtime_error {
 public:
  error(int line, int column, const std::string& message);

  int line() const { return line_; }
  int column() const { return column_; }
  /// @brief The message without the position prefix.
  const std::string& message() const { return message_; }

 private:
  int line_;
  int column_;
  std::string message_;
};

/// @brief The three parts of a source text.
///
/// The two separator lines are kept verbatim (trailing spaces and line
/// ending included) so `compose_document()` reproduces the original bytes.
struct document {
  std::string description;        ///< Part 1: plain text, never parsed.
  std::string format;             ///< Part 2: YAML.
  std::string data;               ///< Part 3: CSV.
  std::string separator1{"--\n"}; ///< The line between parts 1 and 2.
  std::string separator2{"--\n"}; ///< The line between parts 2 and 3.
  int format_line{1};             ///< 1-based source line of `format`'s first line.
  int data_line{1};               ///< 1-based source line of `data`'s first line.
};

/// @brief Split @p text at its first two separator lines (a line that is
///        exactly `--`, trailing spaces/tabs ignored).
/// @throws error if @p text has fewer than two separator lines or exceeds
///         `kMaxSourceBytes`.
document parse_document(std::string_view text);

/// @brief Join a document back into source text; the exact inverse of
///        `parse_document()`.
std::string compose_document(const document& doc);

/// @brief Build a document from three independently edited parts.
///
/// Appends a newline to @p description and @p format when they are
/// non-empty and lack one, so the separator lines stay on lines of their
/// own; sets `format_line`/`data_line` to match the composed text.
document make_document(std::string description, std::string format, std::string data);

} // namespace bdg::wish::nymph
