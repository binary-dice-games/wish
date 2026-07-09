// MIT License © 2025 Binary Dice Games
/// @file ui_importer.cpp
/// @brief Resolves a generic bison::dynamic descriptor tree (see
///        ui_descriptor.hpp) into typed wish UI elements.
#include <ui/ui_importer.hpp>

#include <ui/ui_descriptor.hpp>

#include "src/bison/bison_object.hpp"

#include <stdexcept>

namespace bdg::wish {

using namespace bdg::bison;

// ── Field coercion ────────────────────────────────────────────────────────────

/// Set one field on @p obj from a generic descriptor field, coercing numeric
/// types as needed to match the prototype field type (e.g. integer → float).
static void set_field_from_dynamic(dynamic& obj, key_t field_key, const field& value) {
  auto& dst = obj[field_key];

  if (dst.is<std::monostate>()) {
    // Field not in prototype: accept whatever the descriptor gives us.
    if (value.is<bool>())
      dst = value.as<bool>();
    else if (value.is<int32_t>())
      dst = value.as<int32_t>();
    else if (value.is<float>())
      dst = value.as<float>();
    else if (value.is<std::string>())
      dst = value.as<std::string>();
  } else if (dst.is<bool>() && value.is<bool>()) {
    dst = value.as<bool>();
  } else if (dst.is<int32_t>()) {
    if (value.is<int32_t>())
      dst = value.as<int32_t>();
    else if (value.is<float>())
      dst = static_cast<int32_t>(value.as<float>());
    else if (value.is<std::string>()) {
      // field::operator=(string) uses EnumFlags/Enum attribute to convert;
      // silently ignored when the field has no such attribute.
      try {
        dst = value.as<std::string>();
      } catch (const std::runtime_error&) {
      }
    }
  } else if (dst.is<float>()) {
    if (value.is<float>())
      dst = value.as<float>();
    else if (value.is<int32_t>())
      dst = static_cast<float>(value.as<int32_t>());
  } else if (dst.is<std::string>() && value.is<std::string>()) {
    dst = value.as<std::string>();
  }
  // Type mismatches for unknown fields are silently ignored.
}

// ── Generic dynamic tree → typed wish objects ─────────────────────────────────

static dynamic_ptr build_ui_children(const dynamic& children_node, const std::string& parent_path,
    bool parent_add_to_map, name_map& result) {
  auto children_dyn = dynamic_ptr{key_t{0U}, {}};

  const_cast<dynamic&>(children_node).forEach([&](key_t k, const field& child_field) {
    if (!child_field.is<dynamic_ptr>())
      return;
    const auto& child_ptr = child_field.as<dynamic_ptr>();
    if (!child_ptr)
      return;

    const auto* name_field = child_ptr->findField("__name__"_key);
    bool named = name_field != nullptr && name_field->is<std::string>();
    bool child_add_to_map = parent_add_to_map && named;

    std::string child_path;
    if (child_add_to_map) {
      const std::string& name = name_field->as<std::string>();
      child_path = parent_path.empty() ? name : (parent_path + "." + name);
    }

    ui_element_ptr child = build_ui_node(*child_ptr, child_path, child_add_to_map, result);
    (*children_dyn)[k] = dynamic_ptr{child};
  });

  return children_dyn;
}

ui_element_ptr build_ui_node(const dynamic& node, const std::string& path, bool add_to_map, name_map& result) {
  const auto* type_field = node.findField("__type__"_key);
  if (!type_field || !type_field->is<key_t>()) {
    throw std::runtime_error("wish::build_ui_node: descriptor node missing required \"__type__\" field");
  }
  key_t type_key = type_field->as<key_t>();

  {
    auto lp = dynamic::getRegistry().rlock();
    auto ns_it = lp->find("wish"_key);
    if (ns_it == lp->end() || ns_it->second.find(type_key) == ns_it->second.end()) {
      throw std::runtime_error("wish::build_ui_node: unknown element type");
    }
  }

  ui_element_ptr obj = dynamic::instantiate<ui_element>("wish"_key, type_key);

  const_cast<dynamic&>(node).forEach([&](key_t k, const field& value) {
    if (k == "__type__"_key || k == "__name__"_key || k == "children"_key)
      return;
    set_field_from_dynamic(*obj, k, value);
  });

  const auto* children_field = node.findField("children"_key);
  if (children_field && children_field->is<dynamic_ptr>() && children_field->as<dynamic_ptr>()) {
    obj["children"_key] = build_ui_children(*children_field->as<dynamic_ptr>(), path, add_to_map, result);
    obj->refresh_children_order();
  }

  if (add_to_map) {
    // Stamped so that later cloning the whole tree (ui_template.cpp's
    // instantiate_prototype) can recover each node's dot-path without
    // re-walking a name_map — the clone carries this field along for free.
    obj["__path__"_key] = path;
    result[path] = obj;
  }

  return obj;
}

// ── Public JSON/YAML text entry points ────────────────────────────────────────

ui_tree import_json(const std::string& json_str) {
  dynamic generic = import_descriptor_json(json_str);
  ui_tree result;
  build_ui_node(generic, "", true, result);
  return result;
}

ui_tree import_yaml(const std::string& yaml_str) {
  dynamic generic = import_descriptor_yaml(yaml_str);
  ui_tree result;
  build_ui_node(generic, "", true, result);
  return result;
}

} // namespace bdg::wish
