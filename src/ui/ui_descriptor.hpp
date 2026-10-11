// MIT License © 2025 Binary Dice Games
/// @file ui_descriptor.hpp
/// @brief Client-safe text (JSON/YAML) to generic `bison::dynamic` importer.
///
/// Unlike `wish::import_json`/`wish::import_yaml` (src/ui/ui_importer.hpp), the
/// functions here never touch the "wish" bison class registry: they build a
/// plain, untyped `bison::dynamic` tree straight from the text, so they can
/// run in a client-only binary that has no UI element classes registered
/// (see src/registry.cpp's `register_all()`, called only server-side).
///
/// The resulting tree is what `client::register_template` sends over RMI;
/// the server resolves it into real typed `ui_element` objects (see
/// `wish::build_ui_node`, src/ui/ui_importer.hpp).
#pragma once

#include <i18n/translations.hpp>
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
/// String fields whose whole value starts with `$$` are translation keys:
/// when @p tr is given they are replaced with `translate_text(value, *tr)`
/// (see src/i18n/translations.hpp). Translation happens after parsing, so a
/// translated value may contain any character without breaking the syntax.
///
/// @param json_text  UTF-8 JSON text representing a wish UI hierarchy.
/// @param tr         Optional translations; null leaves strings untouched.
/// @throws std::runtime_error on JSON parse error or a node missing "type".
bison::dynamic import_descriptor_json(const std::string& json_text, const translation_map* tr = nullptr);

/// @brief Parse a YAML descriptor into a generic `bison::dynamic` tree.
/// @param yaml_text  UTF-8 YAML text representing a wish UI hierarchy.
/// @param tr         Optional translations; null leaves strings untouched.
/// @throws std::runtime_error on YAML parse error or a node missing "type".
/// @see import_descriptor_json for the resulting tree shape and translation.
bison::dynamic import_descriptor_yaml(const std::string& yaml_text, const translation_map* tr = nullptr);

/// @brief Sniff leading `{`/`[` vs. YAML and dispatch to the matching parser.
/// @param text  UTF-8 JSON or YAML text representing a wish UI hierarchy.
/// @param tr    Optional translations; null leaves strings untouched.
bison::dynamic import_descriptor_text(const std::string& text, const translation_map* tr = nullptr);

} // namespace bdg::wish
