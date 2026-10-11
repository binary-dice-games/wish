// MIT License © 2026 Binary Dice Games
/// @file yaml_json.hpp
/// @brief YAML/JSON text -> nlohmann::ordered_json, the parser behind
///        `import_descriptor_yaml()`, for code that reads a config file
///        rather than a UI descriptor.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace bdg::wish {

/**
 * @brief Parse YAML or JSON @p text into JSON, keeping mapping key order.
 *
 * Text whose first non-blank character is `{` or `[` is parsed as JSON;
 * anything else as YAML (YAML scalars become strings, booleans, null, int
 * or float, the same way `import_descriptor_yaml()` reads them).
 *
 * @throws std::runtime_error on a syntax error, with the 1-based line and
 *         column of the problem when known.
 */
nlohmann::ordered_json yaml_to_json(const std::string& text);

} // namespace bdg::wish
