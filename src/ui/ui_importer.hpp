// MIT License © 2025 Binary Dice Games
/// @file ui_importer.hpp
/// @brief Builds typed wish UI element trees from a generic bison::dynamic
///        descriptor, and from JSON/YAML text as a convenience wrapper.
///
/// The importer reads a descriptor (either already a generic `bison::dynamic`
/// tree — see src/ui/ui_descriptor.hpp — or JSON/YAML text, which is first
/// parsed into that same generic shape), instantiates every element in the
/// "wish" bison namespace, wires up the children hierarchy, and returns a flat
/// name map for O(1) access to any named node.  Resolving element types
/// against the "wish" class registry is server-only (the registry is
/// populated by `register_all()`, called only from `server.cpp`/
/// `standalone.cpp`), which is why this header depends on `ui_element.hpp`
/// and is only compiled into `wish_server`.
#pragma once

#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"

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

/// @brief Resolve a generic descriptor node (see src/ui/ui_descriptor.hpp) into
///        a typed wish UI element, recursively wiring up its children.
///
/// This is the server-only counterpart of `import_descriptor_json`/
/// `import_descriptor_yaml`: it reads the node's `"__type__"_key` field and
/// instantiates the matching class registered in the "wish" bison namespace
/// (see `register_all()`), coerces every other field's value to match that
/// class's prototype field types, and recurses into `"children"_key`.
///
/// @param node        Generic dynamic node, as produced by
///                    `import_descriptor_json`/`import_descriptor_yaml` or
///                    received as an RMI `register_template` argument.
/// @param path        Dot-path of this node (`""` for the root).
/// @param add_to_map  If true, records this node in @p result under @p path.
/// @param result      Flat dot-path -> element map, populated as nodes are
///                    visited (only entries with `add_to_map == true`).
/// @return The instantiated, fully wired `ui_element` for @p node.
/// @throws std::runtime_error if @p node's type is not a registered class.
ui_element_ptr build_ui_node(const bison::dynamic& node, const std::string& path, bool add_to_map, name_map& result);

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
