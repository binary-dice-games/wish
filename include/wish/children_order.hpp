// MIT License © 2025 Binary Dice Games
/// @file children_order.hpp
/// @brief Render-order utilities for wish UI element children.
///
/// Named children are stored in a bison `dynamic` map keyed by string hash,
/// which iterates in hash-sorted order rather than declaration order.  This
/// header provides two functions that maintain a sorted-key cache so the
/// renderer can visit children in the order they were declared.
///
/// ### Typical usage
///
///   // After importing or mutating `order` fields:
///   refresh_children_order(*my_element);
///
///   // In the renderer:
///   for_each_child_ordered(*parent, [](bison::key_t, const bison::field& f) {
///     render(f.as<bison::dynamic_ptr>());
///   });
#pragma once

#include "src/bison/bison_object.hpp"

#include <functional>

namespace bdg::wish {

/// @brief Rebuild the render-order cache on @p parent.
///
/// Reads each child's `order` field, stable-sorts the children by that value
/// (ascending), and writes the resulting key sequence to an internal
/// `__children_order__` field on @p parent.  Call this once after import and
/// again any time a child's `order` field is changed at runtime.
///
/// @param parent  The UI element whose children should be re-sorted.
void refresh_children_order(bison::dynamic& parent);

/// @brief Iterate the children of @p parent in render order.
///
/// Uses the cache built by `refresh_children_order` when available.  If the
/// cache is absent the children are visited in raw map order (hash-sorted),
/// which is the pre-fix fallback behaviour.
///
/// @param parent  The UI element whose children to iterate.
/// @param fn      Called as `fn(key, field)` for each child in render order.
void for_each_child_ordered(
    const bison::dynamic& parent,
    const std::function<void(bison::key_t, const bison::field&)>& fn);

}  // namespace bdg::wish
