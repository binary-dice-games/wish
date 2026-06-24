// MIT License © 2025 Binary Dice Games
/// @file template_handler.cpp
/// @brief Implementation of the server-side __WishTemplate RMI class.
#include "template_handler.hpp"

#include <wish/ui_importer.hpp>

#include "src/rmi/shared/ids.hpp"

#include <algorithm>
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

  wish::name_map nmap = is_json ? import_json(descriptor) : import_yaml(descriptor);

  bison::dynamic result;
  std::size_t idx = 0;
  for (auto& [name, elem] : nmap) {
    bison::key_t new_id = bison::rmi::shared::generate_id();
    ctx.objects[new_id.id] = elem;
    sess.objects[name] = elem;

    // Store the RMI object ID on the element so the renderer can emit events
    // with the correct ID (not the class name).
    (*elem)["__wish_id"_key] = new_id;

    bison::dynamic entry;
    entry["name"_key] = name;
    entry["id"_key] = new_id;
    result[idx++] = bison::dynamic_ptr{std::move(entry)};
  }
  return result;
}

// ── template_handler ─────────────────────────────────────────────────────────

template_handler::template_handler(bison::dynamic&& base)
    : dynamic(std::move(base)) {}

bison::dynamic template_handler::do_register(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  std::string descriptor = params.as<std::string>("descriptor"_key);
  sess_->templates[name] = std::move(descriptor);
  return dynamic{};
}

bison::dynamic template_handler::do_instantiate(const bison::dynamic& params) {
  bison::key_t name = params.as<bison::key_t>("name"_key);
  auto it = sess_->templates.find(name);
  if (it == sess_->templates.end()) {
    throw std::runtime_error("wish: template not found");
  }
  return apply_descriptor(*ctx_, *sess_, it->second);
}

void register_template_handler() {
  auto proto = bison::dynamic_ptr{"__WishTemplate"_key, {}};
  (void)"name"_rkey;
  (void)"descriptor"_rkey;
  (void)"id"_rkey;
  proto->addMethod("register"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      return static_cast<template_handler&>(s).do_register(p);
    }, attr<DisplayName>("register")});
  proto->addMethod("instantiate"_key, bison::method{
    [](dynamic& s, const dynamic& p) -> dynamic {
      return static_cast<template_handler&>(s).do_instantiate(p);
    }, attr<DisplayName>("instantiate")});
  bison::dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      bison::dynamic::make_factory<template_handler>("wish"_key, "__WishTemplate"_key));
}

}  // namespace bdg::wish
