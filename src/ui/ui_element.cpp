// MIT License © 2025 Binary Dice Games
/// @file ui_element.cpp
/// @brief Implementation of the ui_element base class.
#include <ui/ui_element.hpp>

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
  auto* children_field = cached_field(children_field_, "children"_key);
  if (!children_field || !children_field->is<dynamic_ptr>())
    return;
  auto& children = children_field->as<dynamic_ptr>();
  if (!children)
    return;

  // Collect (key, order_value, child) for every child in the children map.
  struct entry {
    key_t key;
    int32_t order;
    dynamic_ptr child;
  };
  std::vector<entry> entries;
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
    entries.push_back({k, order, child});
  });

  // Stable-sort ascending so equal order values preserve declaration sequence.
  std::stable_sort(entries.begin(), entries.end(),
                    [](const entry& a, const entry& b) { return a.order < b.order; });

  // Write sorted key sequence into "__children_order__" (kept for any
  // external/observability consumers) and rebuild resolved_children_order_,
  // the cache for_each_child_ordered() actually iterates -- resolving each
  // child's ui_element* once here instead of on every for_each_child_ordered()
  // call (1-3x per node per frame during measure/arrange/render). Non-
  // ui_element children (e.g. in a manually-constructed tree) are dropped
  // from the resolved cache, matching for_each_child_ordered()'s previous
  // silent-skip behavior.
  auto cache = dynamic_ptr{key_t{0U}, {}};
  resolved_children_order_.clear();
  resolved_children_order_.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    (*cache)[i] = static_cast<int32_t>(entries[i].key.id);
    if (auto elem = std::dynamic_pointer_cast<ui_element>(std::shared_ptr<dynamic>(entries[i].child)))
      resolved_children_order_.emplace_back(entries[i].key, ui_element_ptr(std::move(elem)));
  }
  has_resolved_children_order_ = true;
  has_menu_bar_child_ = -1; // child set changed; recompute lazily on next query
  (*this)["__children_order__"_key] = cache;
}

} // namespace bdg::wish
