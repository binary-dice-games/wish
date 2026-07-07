// MIT License © 2025 Binary Dice Games
/// @file ui_element.cpp
/// @brief Implementation of the ui_element base class.
#include <ui_element.hpp>

#include "src/bison/bison_object.hpp"

#include <algorithm>
#include <vector>

namespace bdg::wish {

using namespace bdg::bison;

// ── ui_element_ptr ───────────────────────────────────────────────────────────

ui_element_ptr::ui_element_ptr(const std::shared_ptr<ui_element>& that) : std::shared_ptr<ui_element>(that) {}

ui_element_ptr::ui_element_ptr(std::shared_ptr<ui_element>&& that) : std::shared_ptr<ui_element>(std::move(that)) {}

ui_element_ptr::ui_element_ptr(dynamic&& base)
    : std::shared_ptr<ui_element>(std::make_shared<ui_element>(std::move(base))) {}

ui_element_ptr::operator bison::dynamic_ptr() const {
  return bison::dynamic_ptr{std::shared_ptr<dynamic>(*this)};
}

// ── ui_element ───────────────────────────────────────────────────────────────

ui_element::ui_element(dynamic&& base) : cloneable_dynamic(std::move(base)) {}

void ui_element::refresh_children_order() {
  auto* children_field = findField("children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>())
    return;
  auto& children = children_field->as<dynamic_ptr>();
  if (!children)
    return;

  // Collect (key, order_value) for every child in the children map.
  std::vector<std::pair<key_t, int32_t>> entries;
  children->forEach([&](key_t k, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    const auto& child = f.as<dynamic_ptr>();
    int32_t order = 0;
    if (child) {
      auto* of = child->findField("order"_key);
      if (of && of->is<int32_t>())
        order = of->as<int32_t>();
    }
    entries.emplace_back(k, order);
  });

  // Stable-sort ascending so equal order values preserve declaration sequence.
  std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second < b.second; });

  // Write sorted key sequence into the cache.  Each slot i holds the raw
  // hash_t of the child key cast to int32_t (well-defined 2's-complement).
  auto cache = dynamic_ptr{key_t{0U}, {}};
  for (size_t i = 0; i < entries.size(); ++i) {
    (*cache)[i] = static_cast<int32_t>(entries[i].first.id);
  }
  (*this)["__children_order__"_key] = cache;
}

void ui_element::for_each_child_ordered(const std::function<void(key_t, ui_element&)>& fn) const {
  auto* children_field = findField("children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>())
    return;
  const auto& children = children_field->as<dynamic_ptr>();
  if (!children)
    return;

  auto* cache_field = findField("__children_order__"_key);
  if (cache_field && cache_field->is<dynamic_ptr>()) {
    const auto& cache = cache_field->as<dynamic_ptr>();
    if (cache) {
      // Cache entries are indexed 0, 1, 2 ... so forEach visits them in
      // ascending integer order — the intended render order.
      cache->forEach([&](key_t, const field& entry) {
        if (!entry.is<int32_t>())
          return;
        key_t child_key{static_cast<hash_t>(entry.as<int32_t>())};
        auto* child_field = children->findField(child_key);
        if (!child_field || !child_field->is<dynamic_ptr>())
          return;
        auto* elem = dynamic_cast<ui_element*>(child_field->as<dynamic_ptr>().get());
        if (elem)
          fn(child_key, *elem);
      });
      return;
    }
  }

  // Fallback: no cache — use forEachChild<ui_element> (hash-sorted order).
  children->template forEachChild<ui_element>(fn);
}

} // namespace bdg::wish
