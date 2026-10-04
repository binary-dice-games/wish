// MIT License © 2026 Binary Dice Games
/// @file helm_table_parser.hpp
/// @brief Pure string helpers for the helm module's client: parsing `helm`'s
///        table output and validating values that become `helm` arguments.
///
/// `helm list` / `repo list` / `search repo` / `history` have no Go-template
/// or jsonpath output mode, and module-client sources have no JSON library
/// (see the `kubectl` module's DESIGN.md §6), so the default table output is
/// parsed instead: helm writes one row per line with the columns padded by
/// spaces *and* separated by a TAB, so a split on `\t` + a trim recovers each
/// cell exactly, even the ones containing spaces (UPDATED, DESCRIPTION).
///
/// No framework / bison / libuv dependency -- unit-tested directly by
/// tests/test_helm_parsers.cpp.
#pragma once

#include "modules/bdg/dev/common/text.hpp"

#include <string>
#include <vector>

namespace bdg::wish::helm {

/// @brief Parses helm table output into rows of trimmed cells.
///
/// @param text    Raw stdout of the `helm` command.
/// @param header  First-column caption of the header row (e.g. `"NAME"`,
///                `"REVISION"`); a leading row whose first cell equals it is
///                dropped.
/// @param ncols   Every returned row is padded / truncated to this many cells.
/// @return One entry per data row. Blank lines and lines without a TAB (plain
///         messages such as "No results found") are skipped.
std::vector<std::vector<std::string>> parse_table(const std::string& text, const std::string& header, size_t ncols);

/// @brief Shortens `helm list`'s UPDATED cell
/// (`"2024-01-15 10:23:45.123456789 +0000 UTC"`) to `"2024-01-15 10:23:45"`.
/// Anything not in that shape is returned unchanged.
std::string short_timestamp(const std::string& updated);

/// @brief Whether @p value may be passed to `helm` as a positional argument
/// or a flag's value: non-empty and not starting with `-`.
///
/// Every name / namespace / URL / revision the client forwards to `helm`
/// arrives in an event payload. There is no shell to inject into, but a value
/// such as `--post-renderer=/some/binary` would still be parsed by helm as a
/// flag, so anything flag-shaped is rejected before the command is built.
using dev::is_safe_arg;

/// @brief Reduces a failed command's stderr to the part worth showing: from
/// helm's own `Error:` line to the end when there is one (dropping the
/// Kubernetes client warnings / log lines helm prints before it), otherwise
/// the whole text. Trailing newlines are removed.
std::string error_summary(const std::string& stderr_text);

/// @brief Whether @p name is a release name every chart can deploy: a DNS
/// label -- lowercase letters, digits and `-`, starting and ending with a
/// letter or digit, at most 53 characters (helm's own limit).
///
/// helm itself also accepts dots (`v1.0`), but charts put the release name
/// into Service / Pod names, which Kubernetes rejects when they contain one
/// -- after helm has already recorded a failed release.
bool is_valid_release_name(const std::string& name);

} // namespace bdg::wish::helm
