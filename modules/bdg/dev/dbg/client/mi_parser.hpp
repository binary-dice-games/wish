// MIT License © 2026 Binary Dice Games
/// @file mi_parser.hpp
/// @brief Parser for GDB/LLDB Machine Interface (MI) output records.
///
/// `posix_debug_backend` (the Linux/macOS debug_backend implementation)
/// drives a `gdb --interpreter=mi` (or `lldb-mi`) child process and reads
/// its MI record stream: one record per line, each an optional integer
/// token followed by a leading character identifying the record kind
/// (`^` result, `*`/`+`/`=` async, `~`/`@`/`&` stream) and, for the
/// non-stream kinds, a class name and a comma-separated list of
/// `name=value` results whose values are C-strings, `{...}` tuples, or
/// `[...]` lists (the MI output grammar, GDB manual "GDB/MI Output
/// Syntax").
///
/// This file is the pure, platform-agnostic half of that backend: it turns
/// one raw MI line into an `mi_record` tree with no I/O and no OS calls, so
/// it compiles and is unit-tested (`tests/test_mi_parser.cpp`) on every
/// platform, including Windows where `posix_debug_backend` itself is
/// `#if`-d out.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bdg::wish::dbg {

/// @brief One MI value: a C-string constant, a `{...}` tuple, or a `[...]`
///        list. A list may hold bare values or `name=value` results (MI
///        allows both forms); both are kept so callers can index either
///        way.
struct mi_value {
  enum class kind { string, tuple, list };
  kind type{kind::string};

  std::string str; ///< Unescaped text when `type == string`.
  /// `name=value` members, for a tuple or a result-style list.
  std::vector<std::pair<std::string, mi_value>> items;
  std::vector<mi_value> values; ///< Bare elements, for a value-style list.

  /// @brief Returns the member named @p key (searching `items`), or nullptr.
  const mi_value* find(std::string_view key) const;
  /// @brief `find(key)->str` when present and a string, else "".
  std::string get(std::string_view key) const;
};

/// @brief One parsed MI output record.
struct mi_record {
  enum class kind {
    result,         ///< `^done` / `^running` / `^error` / `^connected` / `^exit`
    exec_async,     ///< `*stopped` / `*running`
    status_async,   ///< `+...` (progress)
    notify_async,   ///< `=thread-created`, `=breakpoint-modified`, ...
    console_stream, ///< `~"..."`  (CLI echo)
    target_stream,  ///< `@"..."`  (debuggee stdout via the target)
    log_stream,     ///< `&"..."`  (debugger internal log)
    prompt,         ///< `(gdb)` / `(lldb)` idle marker
    unknown
  };
  kind type{kind::unknown};

  std::string token; ///< Leading digits echoed from the command, or "".
  /// Result/async class (`done`, `stopped`, `thread-created`, ...); for the
  /// three stream kinds, the already-unescaped stream text instead.
  std::string klass;
  std::vector<std::pair<std::string, mi_value>> results;

  const mi_value* find(std::string_view key) const;
  std::string get(std::string_view key) const;

  bool is_error() const {
    return type == kind::result && klass == "error";
  }
};

/// @brief Parses one MI output line (with or without a trailing newline).
/// @return The parsed record, or `std::nullopt` for a blank line or a line
///         whose leading character matches no known record kind (some MI
///         emitters print stray banner text before the first `(gdb)`).
std::optional<mi_record> parse_mi_line(std::string_view line);

/// @brief Decodes a MI C-string body (the text between the quotes) applying
///        the standard C escape sequences GDB emits. Exposed for tests.
std::string mi_unescape(std::string_view quoted_body);

} // namespace bdg::wish::dbg
