// MIT License © 2025 Binary Dice Games
/// @file children_order.cpp
/// @brief Implementation of render-order utilities for wish UI element children.
#include <wish/children_order.hpp>

#include "src/bison/bison_object.hpp"

#include <algorithm>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

void refresh_children_order(dynamic& parent) {
  auto* children_field = parent.findField("children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>()) return;
  auto& children = children_field->as<dynamic_ptr>();
  if (!children) return;

  // Collect (key, order_value) for every child in the children map.
  std::vector<std::pair<key_t, int32_t>> entries;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>()) return;
    const auto& child = f.as<dynamic_ptr>();
    int32_t order = 0;
    if (child) {
      auto* of = child->findField("order"_key);
      if (of && of->is<int32_t>()) order = of->as<int32_t>();
    }
    entries.emplace_back(k, order);
  });

  // Stable-sort ascending by order value so equal values preserve source order.
  std::stable_sort(entries.begin(), entries.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });

  // Write sorted key sequence into the cache.  Each entry i holds the raw
  // hash_t of the child key, cast to int32_t for storage as a bison field.
  auto cache = dynamic_ptr{key_t{0U}, {}};
  for (size_t i = 0; i < entries.size(); ++i) {
    (*cache)[i] = static_cast<int32_t>(entries[i].first.id);
  }
  parent["__children_order__"_key] = cache;
}

void for_each_child_ordered(
    const dynamic& parent,
    const std::function<void(key_t, const field&)>& fn) {

  auto* children_field = parent.findField("children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>()) return;
  const auto& children = children_field->as<dynamic_ptr>();
  if (!children) return;

  auto* cache_field = parent.findField("__children_order__"_key);
  if (cache_field && cache_field->is<dynamic_ptr>()) {
    const auto& cache = cache_field->as<dynamic_ptr>();
    if (cache) {
      // Cache entries are indexed 0, 1, 2... so forEach visits them in
      // ascending integer order, which is the intended render order.
      cache->forEach([&](key_t, const field& entry) {
        if (!entry.is<int32_t>()) return;
        key_t child_key{static_cast<hash_t>(entry.as<int32_t>())};
        auto* child_field = children->findField(child_key);
        if (child_field) fn(child_key, *child_field);
      });
      return;
    }
  }

  // Fallback: no cache — visit dynamic_ptr children in raw map order.
  children->forEach([&](key_t k, const field& f) {
    if (f.is<dynamic_ptr>()) fn(k, f);
  });
}

}  // namespace bdg::wish
