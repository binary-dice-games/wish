// MIT License © 2026 Binary Dice Games
/// @file style_rpc.hpp
/// @brief Client-side calls into the server's `__WishStyle` service that
///        `wish::client`, `wish::standalone` and `wish desktop` share.
#pragma once

#include <context/style_service.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/client/proxy.hpp"

#include <string>
#include <vector>

namespace bdg::wish::detail {

/// @brief Synchronous `themes`: the themes the server's renderer has, in
///        its order (see `registered_themes()`); empty for a server that
///        predates the method.
inline std::vector<theme_info> list_themes_sync(bison::rmi::proxy::dynamic& style_proxy) {
  using namespace bison;
  std::vector<theme_info> out;
  dynamic list;
  try {
    list = style_proxy.call("themes"_key, dynamic{}).get();
  } catch (...) {
    return out; // an older server: no such method
  }
  for (uint32_t i = 0;; ++i) {
    const auto* f = list.findField(key_t{i});
    if (!f || !f->is<dynamic_ptr>() || !f->as<dynamic_ptr>())
      break;
    const dynamic& item = *f->as<dynamic_ptr>();
    theme_info t;
    t.name = item.get_as<std::string>("name"_key, std::string{});
    t.label = item.get_as<std::string>("label"_key, std::string{});
    t.is_light = item.get_as<bool>("light"_key, false);
    if (!t.name.empty())
      out.push_back(std::move(t));
  }
  return out;
}

} // namespace bdg::wish::detail
