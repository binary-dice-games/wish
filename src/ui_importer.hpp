// MIT License © 2025 Binary Dice Games
/// @file ui_importer.hpp
/// @brief Parses JSON or YAML descriptors into live wish object trees.
///
/// The importer reads a descriptor string, instantiates every element in the
/// "wish" bison namespace, wires up the children hierarchy, and returns a flat
/// name map for O(1) access to any named node.
#pragma once

#include <ui_element.hpp>

#include <string>
#include <unordered_map>

namespace bdg::wish {

/// @brief Flat map from dot-path name to the corresponding wish UI element.
///
/// The root node is stored at key `""`.  Named descendants use dot-joined
/// ancestor names, e.g. `"body.row.ok"`.  Indexed (numeric) children are
/// not included; they are accessible via integer index on the parent's
/// `children` field.
using name_map = std::unordered_map<std::string, ui_element_ptr>;

/// @brief Returned by import_json / import_yaml; extends name_map with with().
class ui_tree : public name_map {
 public:
  /// @brief Invoke fn(elem) if path exists in the tree; no-op otherwise.
  /// @param path  Dot-joined element path, e.g. `"vbox.btn_row.btn_open"`.
  /// @param fn    Callable accepting a `const ui_element_ptr&`.
  template <typename Fn>
  void with(const std::string& path, Fn&& fn) const {
    if (auto it = find(path); it != end())
      fn(it->second);
  }

  /// @brief Move all entries from @p source into this tree, prepending @p
  /// prefix to each key.
  ///
  /// The root entry (key `""`) of @p source is stored at @p prefix; all other
  /// entries are stored at `prefix + "." + key`.  The source tree is left in
  /// a valid but unspecified state after the call.
  /// @param source  Donor tree (e.g. the result of import_json / import_yaml).
  /// @param prefix  Non-empty prefix used as the root key in this tree.
  void merge(ui_tree&& source, const std::string& prefix) {
    for (auto& [key, ptr] : source)
      (*this)[key.empty() ? prefix : (prefix + "." + key)] = std::move(ptr);
  }
};

/// @brief Parse a JSON descriptor and return a wish object tree.
/// @param json  UTF-8 JSON text representing a wish UI hierarchy.
/// @return ui_tree with root at `""` and all named descendants by dot-path.
/// @throws std::runtime_error on JSON parse error or unknown element type.
ui_tree import_json(const std::string& json);

/// @brief Parse a YAML descriptor and return a wish object tree.
/// @param yaml  UTF-8 YAML text representing a wish UI hierarchy.
/// @return ui_tree with root at `""` and all named descendants by dot-path.
/// @throws std::runtime_error on YAML parse error or unknown element type.
ui_tree import_yaml(const std::string& yaml);

} // namespace bdg::wish
