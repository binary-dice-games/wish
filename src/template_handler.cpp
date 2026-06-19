// MIT License © 2025 Binary Dice Games
/// @file template_handler.cpp
/// @brief Implementation of the server-side __WishTemplate RMI class.
#include "template_handler.hpp"

#include "import_handler.hpp"

#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── template_handler ─────────────────────────────────────────────────────────

template_handler::template_handler(bison::dynamic&& base)
    : wish_handler(std::move(base)) {
  addMethod(
      "register"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        bison::key_t name = params.as<bison::key_t>("name"_key);
        std::string descriptor = params.as<std::string>("descriptor"_key);
        sess_->templates[name] = std::move(descriptor);
        return dynamic{};
      }});

  addMethod(
      "instantiate"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        bison::key_t name = params.as<bison::key_t>("name"_key);
        auto it = sess_->templates.find(name);
        if (it == sess_->templates.end()) {
          throw std::runtime_error("wish: template not found");
        }
        return apply_descriptor(*ctx_, *sess_, it->second);
      }});
}

void register_template_handler() {
  auto proto = bison::dynamic::instantiate<template_handler>(
      key_t{0U}, "__WishTemplate"_key);
  bison::dynamic::addClass(
      "wish"_key,
      bison::dynamic_ptr{proto},
      key_t{0U},
      bison::dynamic::make_factory<template_handler>("wish"_key, "__WishTemplate"_key));
}

}  // namespace bdg::wish
