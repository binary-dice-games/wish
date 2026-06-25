// MIT License © 2025 Binary Dice Games
/// @file ui_element.hpp
/// @brief Typed C++ base class for all wish UI elements.
///
/// `ui_element` inherits from `bison::dynamic` so that element-specific logic
/// (render ordering, future helpers) lives as member functions rather than
/// free functions operating on raw `dynamic` objects.
///
/// All wish element instances created by `ui_importer` are `ui_element` objects.
///
/// `ui_element_ptr` inherits from `std::shared_ptr<ui_element>`, giving it all
/// standard shared-pointer operations.  It adds `operator[]` so field access can
/// be written as `ptr["key"_key] = value` instead of `(*ptr)["key"_key] = value`,
/// and an implicit conversion to `bison::dynamic_ptr` so `ui_element_ptr` values
/// can be stored in bison fields without explicit casting.
///
/// ### Typical usage
///
///   auto result = wish::import_json(desc);      // name_map<string, ui_element_ptr>
///   auto& win = result[""];
///   win->refresh_children_order();              // called automatically at import
///   win["title"_key] = "Hello";                // no (*win) needed
///
///   win->for_each_child_ordered([](bison::key_t, wish::ui_element& child) {
///     render(child);
///   });
#pragma once

#include "src/bison/bison_object.hpp"

#include <functional>
#include <memory>

namespace bdg::wish {

class ui_element;

/**
 * @brief Smart pointer for wish UI elements; inherits `std::shared_ptr<ui_element>`.
 *
 * All standard shared-pointer operations (`->`, `*`, `bool`, `get()`,
 * comparisons, `reset()`, `swap()`, …) are inherited.  Two extras are added:
 *   - `operator[]` so field assignments read as `ptr["key"_key] = val`
 *     rather than `(*ptr)["key"_key] = val`.
 *   - Implicit conversion to `bison::dynamic_ptr` for storage in bison fields.
 */
class ui_element_ptr : public std::shared_ptr<ui_element> {
 public:
  using std::shared_ptr<ui_element>::shared_ptr;
  using std::shared_ptr<ui_element>::operator=;

  ui_element_ptr(const std::shared_ptr<ui_element>& that);
  ui_element_ptr(std::shared_ptr<ui_element>&& that);

  /// @brief Construct by upgrading a plain bison::dynamic rvalue into a ui_element.
  explicit ui_element_ptr(bison::dynamic&& base);

  /// @brief Field access: `ptr["key"_key] = value` without needing `(*ptr)`.
  template<typename K>
  decltype(auto) operator[](K key) const { return (**this)[key]; }

  /// @brief Implicit conversion to bison::dynamic_ptr for field assignments
  ///        and bison APIs that store elements by dynamic_ptr.
  operator bison::dynamic_ptr() const;  // NOLINT(google-explicit-constructor)
};

/**
 * @brief Typed base class for all wish UI elements.
 *
 * Inherits from `bison::dynamic` and adds wish-specific member functions.
 * Instances are created by `ui_importer` via
 * `bison::dynamic::instantiate<ui_element>(...)`.
 */
class ui_element : public bison::dynamic {
 public:
  /**
   * @brief Construct a `ui_element` by moving a plain `dynamic` into it.
   *
   * Used by `dynamic::instantiate<ui_element>()` to upgrade the base object
   * returned by the non-template overload.
   *
   * @param base  `dynamic` object produced by `dynamic::instantiate(ns, klass)`.
   */
  explicit ui_element(bison::dynamic&& base);

  /**
   * @brief Rebuild the render-order cache from children's `order` fields.
   *
   * Reads each child's `order` field, stable-sorts the children ascending, and
   * writes the result to `__children_order__` on this element.  Called
   * automatically at import time; call again after mutating any child's `order`
   * field at runtime.
   */
  void refresh_children_order();

  /**
   * @brief Iterate children in render order.
   *
   * Uses the cache built by `refresh_children_order` when available; falls back
   * to hash-sorted map order otherwise.  Children that are not `ui_element`
   * instances (e.g. in manually-constructed trees) are silently skipped.
   *
   * @param fn  Called as `fn(key, child)` for each child in render order.
   */
  void for_each_child_ordered(
      const std::function<void(bison::key_t, ui_element&)>& fn) const;
};

}  // namespace bdg::wish
