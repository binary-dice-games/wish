// MIT License © 2025 Binary Dice Games
/// @file ui_importer.hpp
/// @brief Parses JSON or YAML descriptors into live wish object trees.
///
/// The importer reads a descriptor string, instantiates every element in the
/// "wish" bison namespace, wires up the children hierarchy, and returns a flat
/// name map for O(1) access to any named node.
#pragma once

#include "src/bison/bison_object.hpp"

#include <string>
#include <unordered_map>

namespace bdg::wish {

/// @brief Flat map from dot-path name to the corresponding wish object.
///
/// The root node is stored at key `""`.  Named descendants use dot-joined
/// ancestor names, e.g. `"body.row.ok"`.  Indexed (numeric) children are
/// not included; they are accessible via integer index on the parent's
/// `children` field.
using name_map = std::unordered_map<std::string, bison::dynamic_ptr>;

/// @brief Parse a JSON descriptor and return a wish object tree.
/// @param json  UTF-8 JSON text representing a wish UI hierarchy.
/// @return name_map with root at `""` and all named descendants by dot-path.
/// @throws std::runtime_error on JSON parse error or unknown element type.
name_map import_json(const std::string& json);

/// @brief Parse a YAML descriptor and return a wish object tree.
/// @param yaml  UTF-8 YAML text representing a wish UI hierarchy.
/// @return name_map with root at `""` and all named descendants by dot-path.
/// @throws std::runtime_error on YAML parse error or unknown element type.
name_map import_yaml(const std::string& yaml);

}  // namespace bdg::wish
