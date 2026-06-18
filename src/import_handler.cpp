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
  // Auto-detect format: JSON starts with '{' or '['.
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

void import_handler::register_class() {
  auto proto = dynamic_ptr{"__WishImport"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

import_handler::import_handler(
    bison::dynamic&& base,
    bison::rmi::context& ctx,
    std::shared_ptr<session> sess)
    : dynamic(std::move(base)), ctx_(ctx), sess_(std::move(sess)) {
  addMethod(
      "import"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        std::string descriptor = params.as<std::string>("descriptor"_key);
        return apply_descriptor(ctx_, *sess_, descriptor);
      }});
}

}  // namespace bdg::wish
