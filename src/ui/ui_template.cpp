// MIT License © 2025 Binary Dice Games
/// @file ui_template.cpp
/// @brief Implementation of the server-side __WishTemplate RMI class.
#include <ui/ui_template.hpp>

#include <ui/ui_importer.hpp>
#include <ui/ui_root.hpp>

#include "src/rmi/shared/ids.hpp"

#include <atomic>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── instantiate_prototype ────────────────────────────────────────────────────

// Walk a (freshly cloned) element tree, assigning each mapped node (one
// stamped with "__path__" by build_ui_node at register time) a fresh RMI id
// and registering it into ctx/sess, appending a {name, id} entry to `result`.
static void collect_ids(rmi::context& ctx, context& sess, const ui_element_ptr& node, dynamic& result, size_t& idx) {
  const auto* path_field = node->findField("__path__"_key);
  if (path_field && path_field->is<std::string>()) {
    const std::string& path = path_field->as<std::string>();
    key_t new_id = rmi::shared::generate_id();
    ctx.objects[new_id.id] = node;
    sess.ui_objects[path] = node;

    // Store the RMI object ID on the element so the renderer can emit events
    // with the correct ID (not the class name).
    (*node)["__wish_id"_key] = new_id;

    dynamic entry;
    entry["name"_key] = path;
    entry["id"_key] = new_id;
    result[idx++] = dynamic_ptr{std::move(entry)};
  }

  const auto* children_field = node->findField("children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>())
    return;
  const auto& children = children_field->as<dynamic_ptr>();
  if (!children)
    return;

  children->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    const auto& child_dyn = f.as<dynamic_ptr>();
    if (!child_dyn)
      return;
    auto* child_elem = dynamic_cast<ui_element*>(child_dyn.get());
    if (!child_elem)
      return;
    // Alias the child's ownership block so recursion keeps working with a
    // typed ui_element_ptr instead of the generic dynamic_ptr field value.
    ui_element_ptr child_ptr{std::shared_ptr<ui_element>(std::shared_ptr<dynamic>(child_dyn), child_elem)};
    collect_ids(ctx, sess, child_ptr, result, idx);
  });
}

// Deep-clone the stored template prototype, assign every node a fresh RMI id,
// and register the root as a top-level renderable (and, if it's a ui_root
// subtype, as its own event handler). Returns the {name, id} entry array the
// client uses to build its local proxy_map.
static dynamic instantiate_prototype(rmi::context& ctx, context& sess, const ui_element_ptr& prototype) {
  ui_element_ptr cloned_root{std::static_pointer_cast<ui_element>(std::shared_ptr<dynamic>(prototype->clone_ptr()))};

  dynamic result;
  size_t idx = 0;
  collect_ids(ctx, sess, cloned_root, result, idx);

  // Generate a unique top-level key for this instantiation so multiple
  // templates can coexist without overwriting each other.
  static std::atomic<uint32_t> tpl_counter{0};
  const key_t tpl_key{"__tpl_" + std::to_string(tpl_counter.fetch_add(1))};

  sess.top_level_objects[tpl_key] = cloned_root;
  if (auto* root_iface = dynamic_cast<ui_root*>(cloned_root.get()))
    sess.top_level_handlers[tpl_key] = root_iface;

  return result;
}

// ── ui_template ─────────────────────────────────────────────────────────

ui_template::ui_template(bison::dynamic&& base) : dynamic(std::move(base)) {}

bison::dynamic ui_template::do_register(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  const auto& descriptor_ptr = params.as<bison::dynamic_ptr>("descriptor"_key);
  if (!descriptor_ptr) {
    throw std::runtime_error("wish: register_template requires a descriptor");
  }

  // Resolve element types against the "wish" registry once, here — the
  // stored prototype is a fully-typed tree, so instantiate_template only
  // needs to clone it and assign fresh ids, not re-resolve anything.
  name_map nm; // discarded; only drives build_ui_node's "__path__" stamping
  sess().templates[name] = build_ui_node(*descriptor_ptr, "", true, nm);
  return dynamic{};
}

bison::dynamic ui_template::do_instantiate(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  context& s = sess();
  auto it = s.templates.find(name);
  if (it == s.templates.end()) {
    throw std::runtime_error("wish: template not found");
  }
  return instantiate_prototype(*ctx_, s, it->second);
}

void register_ui_template() {
  auto proto = bison::dynamic_ptr{"__WishTemplate"_key, {}};

  auto reg_in = std::make_shared<dynamic>();
  reg_in->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  reg_in->addField("descriptor"_key, field{dynamic_ptr{}, attr<DisplayName>("descriptor")});
  proto->addMethod(
      "register"_key,
      bison::method{
          [](dynamic& s, const dynamic& p) -> dynamic { return static_cast<ui_template&>(s).do_register(p); },
          dynamic_ptr{reg_in},
          nullptr,
          attr<DisplayName>("register")});

  auto inst_in = std::make_shared<dynamic>();
  inst_in->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  auto inst_out = std::make_shared<dynamic>();
  inst_out->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  inst_out->addField("id"_key, field{key_t{}, attr<DisplayName>("id")});
  proto->addMethod(
      "instantiate"_key,
      bison::method{
          [](dynamic& s, const dynamic& p) -> dynamic { return static_cast<ui_template&>(s).do_instantiate(p); },
          dynamic_ptr{inst_in},
          dynamic_ptr{inst_out},
          attr<DisplayName>("instantiate")});
  bison::dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      bison::dynamic::make_factory<ui_template>("wish"_key, "__WishTemplate"_key));
}

} // namespace bdg::wish
