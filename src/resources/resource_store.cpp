// MIT License © 2025 Binary Dice Games
/// @file resource_store.cpp
/// @brief Read-only resource store backed by the generated resource table.
#include <wish/resource_store.hpp>

#include "resource_entry.hpp"

#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>

namespace bdg::wish {

// Defined in the generated embedded_resources.cpp translation unit.
extern const resource_entry g_resource_table[];
extern const std::size_t g_resource_count;

static constexpr std::string_view kScheme = "res://";

namespace resource_store {

bool is_resource_path(std::string_view path) {
  return path.substr(0, kScheme.size()) == kScheme;
}

std::string_view strip_scheme(std::string_view path) {
  if (path.substr(0, kScheme.size()) == kScheme)
    return path.substr(kScheme.size());
  return path;
}

std::optional<resource_view> find(std::string_view path) {
  for (std::size_t i = 0; i < g_resource_count; ++i) {
    if (path == g_resource_table[i].path)
      return resource_view{{g_resource_table[i].data, g_resource_table[i].size}};
  }
  return std::nullopt;
}

} // namespace resource_store

} // namespace bdg::wish
