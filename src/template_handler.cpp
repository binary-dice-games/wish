// MIT License © 2025 Binary Dice Games
/// @file template_handler.cpp
/// @brief Implementation of the server-side __WishTemplate RMI class.
#include "template_handler.hpp"

#include <wish/ui_importer.hpp>
#include <wish/top_level_element.hpp>

#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── apply_descriptor ─────────────────────────────────────────────────────────

static bison::dynamic apply_descriptor(
    bison::rmi::context& ctx,
    session& sess,
    const std::string& descriptor) {
  auto it = std::find_if_not(descriptor.cbegin(), descriptor.cend(),
                              [](unsigned char c) { return std::isspace(c); });
  bool is_json =
      (it != descriptor.cend() && (*it == '{' || *it == '['));

  auto nmap = is_json ? import_json(descriptor) : import_yaml(descriptor);

  // Generate a unique top-level key for this instantiation so multiple
  // templates can coexist without overwriting each other.
  static std::atomic<uint32_t> tpl_counter{0};
  const std::string tpl_key =
      "__tpl_" + std::to_string(tpl_counter.fetch_add(1));

  bison::dynamic result;
  std::size_t idx = 0;
  for (auto& [name, elem] : nmap) {
    bison::key_t new_id = bison::rmi::shared::generate_id();
    ctx.objects[new_id.id] = elem;
    sess.objects[name] = elem;

    // Store the RMI object ID on the element so the renderer can emit events
    // with the correct ID (not the class name).
    elem["__wish_id"_key] = new_id;

    bison::dynamic entry;
    entry["name"_key] = name;
    entry["id"_key] = new_id;
    result[idx++] = bison::dynamic_ptr{std::move(entry)};
  }

  // Register the root element as a top-level renderable and its own event handler.
  // The root is at key "" in the name_map (the outermost element of the descriptor).
  auto root_it = nmap.find("");
  if (root_it != nmap.end()) {
    sess.top_level_objects[tpl_key]  = root_it->second;
    sess.top_level_handlers[tpl_key] = root_it->second.get();
  }

  return result;
}

// ── template_handler ─────────────────────────────────────────────────────────

template_handler::template_handler(bison::dynamic&& base)
    : dynamic(std::move(base)) {}

bison::dynamic template_handler::do_register(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  std::string descriptor = params.as<std::string>("descriptor"_key);
  sess().templates[name] = std::move(descriptor);
  return dynamic{};
}

bison::dynamic template_handler::do_instantiate(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  session& s = sess();
  auto it = s.templates.find(name);
  if (it == s.templates.end()) {
    throw std::runtime_error("wish: template not found");
  }
  return apply_descriptor(*ctx_, s, it->second);
}

void register_template_handler() {
  auto proto = bison::dynamic_ptr{"__WishTemplate"_key, {}};

  auto reg_in = std::make_shared<dynamic>();
  reg_in->addField("name"_key,       field{std::string{}, attr<DisplayName>("name")});
  reg_in->addField("descriptor"_key, field{std::string{}, attr<DisplayName>("descriptor")});
  proto->addMethod("register"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      return static_cast<template_handler&>(s).do_register(p);
    },
    dynamic_ptr{reg_in}, nullptr,
    attr<DisplayName>("register")});

  auto inst_in = std::make_shared<dynamic>();
  inst_in->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  auto inst_out = std::make_shared<dynamic>();
  inst_out->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  inst_out->addField("id"_key,   field{key_t{},       attr<DisplayName>("id")});
  proto->addMethod("instantiate"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      return static_cast<template_handler&>(s).do_instantiate(p);
    },
    dynamic_ptr{inst_in}, dynamic_ptr{inst_out},
    attr<DisplayName>("instantiate")});
  bison::dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      bison::dynamic::make_factory<template_handler>("wish"_key, "__WishTemplate"_key));
}

}  // namespace bdg::wish
