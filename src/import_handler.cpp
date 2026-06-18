// MIT License © 2025 Binary Dice Games
/// @file import_handler.cpp
/// @brief Implementation of the server-side __WishImport RMI class.
#include "import_handler.hpp"

#include <wish/ui_importer.hpp>

#include "src/rmi/shared/ids.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── apply_descriptor ─────────────────────────────────────────────────────────

bison::dynamic apply_descriptor(
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

    bison::dynamic entry;
    entry["name"_key] = name;
    entry["id"_key] = new_id;
    result[idx++] = bison::dynamic_ptr{std::move(entry)};
  }
  return result;
}

// ── import_handler ───────────────────────────────────────────────────────────

import_handler::import_handler(bison::dynamic&& base)
    : wish_handler(std::move(base)) {
  addMethod(
      "import"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        std::string descriptor = params.as<std::string>("descriptor"_key);
        return apply_descriptor(*ctx_, *sess_, descriptor);
      }});
}

bison::dynamic_ptr import_handler::clone_for_instance() const {
  return bison::dynamic::instantiate<import_handler>("wish"_key, "__WishImport"_key);
}

void register_import_handler() {
  auto proto = bison::dynamic::instantiate<import_handler>(
      key_t{0U}, "__WishImport"_key);
  bison::dynamic::addClass("wish"_key, bison::dynamic_ptr{proto});
}

}  // namespace bdg::wish
