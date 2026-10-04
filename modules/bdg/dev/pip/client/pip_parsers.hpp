// MIT License © 2026 Binary Dice Games
/// @file pip_parsers.hpp
/// @brief Pure string helpers for the pip module's client: parsing `pip`'s
///        output and validating values that become `pip` arguments.
///
/// Module-client sources have no JSON library (see the `kubectl` module's
/// DESIGN.md §6), so `pip list --format=json` -- a flat array of objects with
/// scalar values -- is read by the small dedicated parser below rather than
/// by scraping pip's column output, whose last column (an editable project's
/// location) may contain spaces.
///
/// No framework / bison / libuv dependency -- unit-tested directly by
/// tests/test_pip_parsers.cpp.
#pragma once

#include "modules/bdg/dev/common/text.hpp"

#include <map>
#include <string>
#include <vector>

namespace bdg::wish::pip {

/// @brief One JSON object as a key -> value map. String values are unescaped;
/// numbers / `true` / `false` / `null` keep their literal text; nested
/// objects and arrays are skipped (the key is absent).
using json_object = std::map<std::string, std::string>;

/// @brief Parses a JSON array of flat objects (`pip list --format=json`).
/// @return One entry per object, in order. Malformed input yields the objects
///         read before the error (an empty vector for non-JSON text).
std::vector<json_object> parse_json_objects(const std::string& text);

/// @brief The parts of `pip index versions <name>` output worth showing.
struct index_versions {
  std::vector<std::string> versions; // newest first, as pip lists them
  std::string installed;             // "" when the package is not installed
  std::string latest;
};

/// @brief Parses `pip index versions <name>` stdout:
/// ```
/// requests (2.34.2)
/// Available versions: 2.34.2, 2.34.1, ...
///   INSTALLED: 2.32.5
///   LATEST:    2.34.2
/// ```
/// Missing parts are left empty.
index_versions parse_index_versions(const std::string& text);

/// @brief Whether @p value may be passed to `pip` as a positional argument
/// or a flag's value: non-empty and not starting with `-`.
///
/// Every package spec / path the client forwards to `pip` arrives in an
/// event payload. There is no shell to inject into, but a value such as
/// `--index-url=http://attacker.example` would still be parsed by pip as an
/// option, so anything flag-shaped is rejected before the command is built.
using dev::is_safe_arg;

/// @brief Whether @p name is a valid distribution name (PEP 508): letters,
/// digits, `.`, `_` and `-`, starting and ending with a letter or digit.
/// Used for the commands that take a bare name (uninstall / show / index).
bool is_valid_package_name(const std::string& name);

/// @brief Splits an install box's text into requirement arguments on
/// whitespace: `"numpy  pandas==2.2"` -> `{"numpy", "pandas==2.2"}`.
std::vector<std::string> split_requirements(const std::string& text);

/// @brief Reduces a failed command's stderr to the part worth showing: from
/// pip's first `ERROR:` line to the end when there is one (dropping the
/// `WARNING:` lines and build chatter before it), otherwise the whole text.
/// Trailing newlines are removed. A PEP 668 `externally-managed-environment`
/// refusal is replaced by a one-line explanation pointing at a virtualenv.
std::string error_summary(const std::string& stderr_text);

} // namespace bdg::wish::pip
