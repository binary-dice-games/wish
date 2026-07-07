// MIT License © 2025 Binary Dice Games
/// @file ui_descriptor.hpp
/// @brief Client-safe text (JSON/YAML) to generic `bison::dynamic` importer.
///
/// Unlike `wish::import_json`/`wish::import_yaml` (src/ui_importer.hpp), the
/// functions here never touch the "wish" bison class registry: they build a
/// plain, untyped `bison::dynamic` tree straight from the text, so they can
/// run in a client-only binary that has no UI element classes registered
/// (see src/registry.cpp's `register_all()`, called only server-side).
///
/// The resulting tree is what `client::register_template` sends over RMI;
/// the server resolves it into real typed `ui_element` objects (see
/// `wish::build_ui_node`, src/ui_importer.hpp).
#pragma once

#include "src/bison/bison_object.hpp"

#include <string>

namespace bdg::wish {

/// @brief Parse a JSON descriptor into a generic `bison::dynamic` tree.
///
/// Each node becomes a `bison::dynamic` with:
/// - `"__type__"_key`  — `bison::key_t` hash of the node's `"type"` string.
/// - `"__name__"_key`  — original string name (named children only).
/// - `"order"_key`     — declaration-order `int32_t`, stamped if not given.
/// - `"children"_key`  — nested `dynamic_ptr`, index-keyed for arrays or
///   `key_t{name}`-keyed for named objects.
/// - all other scalar fields, copied by their natural JSON type (no
///   prototype-based coercion — that happens server-side).
///
/// @param json_text  UTF-8 JSON text representing a wish UI hierarchy.
/// @throws std::runtime_error on JSON parse error or a node missing "type".
bison::dynamic import_descriptor_json(const std::string& json_text);

/// @brief Parse a YAML descriptor into a generic `bison::dynamic` tree.
/// @param yaml_text  UTF-8 YAML text representing a wish UI hierarchy.
/// @throws std::runtime_error on YAML parse error or a node missing "type".
/// @see import_descriptor_json for the resulting tree shape.
bison::dynamic import_descriptor_yaml(const std::string& yaml_text);

/// @brief Sniff leading `{`/`[` vs. YAML and dispatch to the matching parser.
/// @param text  UTF-8 JSON or YAML text representing a wish UI hierarchy.
bison::dynamic import_descriptor_text(const std::string& text);

} // namespace bdg::wish
