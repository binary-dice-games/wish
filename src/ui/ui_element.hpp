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
 * @brief A two-float pair, shaped like ImGui's `ImVec2` but with no ImGui
 *        dependency, so this header stays usable in builds without
 *        `WISH_ENABLE_IMGUI`. Used for both a size and a position -- the
 *        first component is width/x, the second height/y depending on
 *        context.
 */
struct vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

/**
 * @brief Transient, per-frame layout geometry computed by the ImGui
 *        renderer's measure/arrange pass (`src/imgui/imgui_layout.cpp`).
 *
 * Recomputed every frame -- never serialized, never RMI-visible (unlike the
 * `std::map<key_t,field>` dynamic fields other `ui_element` state lives in),
 * and not preserved across `clone()`: `cloneable_dynamic<Derived>::
 * clone_ptr()` default-constructs a fresh `Derived` and only copies
 * `dynamic`'s own field map into it, so a cloned node always starts with
 * `arranged_frame == -1` (never arranged) until the next measure/arrange
 * pass runs -- exactly the "self-heal" behavior `is_arrange_fresh()` is
 * designed to produce anyway.
 */
struct layout_stash {
  vec2f measured; ///< Node's own natural (intrinsic) size.
  vec2f last_rendered_size; ///< This node's own real, ImGui-computed item
                             ///< rect size from the last time it was
                             ///< actually rendered (`imgui_renderer::
                             ///< render_node()`'s generic post-dispatch
                             ///< rect capture -- the same one automation/
                             ///< highlight already use). `measure_node()`
                             ///< falls back to this for any class with no
                             ///< registered `measure_fn`, instead of
                             ///< assuming `{0,0}` -- self-correcting within
                             ///< one frame of any real size change, exactly
                             ///< like the pre-refactor `layout_height_cache`,
                             ///< but automatically correct for every widget
                             ///< class, present and future, with no
                             ///< per-class formula to write or keep in sync.
  vec2f arranged_pos; ///< Resolved top-left, relative to the parent's
                       ///< content-region cursor-start origin.
  vec2f arranged_size; ///< Resolved content box size -- the space this node
                        ///< was *given* by its parent (or, for a self-healed
                        ///< root, whatever the ambient avail happened to be),
                        ///< not necessarily how much of it children actually
                        ///< used. See `content_extent` for that.
  vec2f content_extent; ///< For `VerticalLayout`/`HorizontalLayout` only: how
                         ///< much space this node's own children actually
                         ///< consumed after arrangement (<= `arranged_size`
                         ///< on each axis). Equals `arranged_size` whenever a
                         ///< stretch/fill child soaks up the remainder (the
                         ///< common case for a real Layout row/column), but
                         ///< can be much smaller when a node self-heals in a
                         ///< context with no meaningful bound (e.g. inside a
                         ///< Table cell, where the ambient available height
                         ///< is "however much of the table's scroll region is
                         ///< left", not this node's own row) and none of its
                         ///< children stretch to fill it.
  int arranged_frame = -1; ///< `ImGui::GetFrameCount()` at the last arrange
                            ///< write; -1 means "never arranged".
};

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
  template <typename K>
  decltype(auto) operator[](K key) const {
    return (**this)[key];
  }

  /// @brief Implicit conversion to bison::dynamic_ptr for field assignments
  ///        and bison APIs that store elements by dynamic_ptr.
  operator bison::dynamic_ptr() const; // NOLINT(google-explicit-constructor)
};

/**
 * @brief Typed base class for all wish UI elements.
 *
 * Inherits from `bison::cloneable_dynamic<ui_element>` (rather than
 * `bison::dynamic` directly) so that cloning a tree of `ui_element`s — e.g.
 * `bison::dynamic::clone()`/`clone_ptr()` on a nested `dynamic_ptr` field —
 * reconstructs `ui_element` instances at every level instead of slicing them
 * down to plain `dynamic`, which would break `dynamic_cast<ui_element*>`
 * (used by `for_each_child_ordered`) on the clone. See
 * `bison::cloneable_dynamic` for how the mixin works.
 *
 * Instances are created by `ui_importer` via
 * `bison::dynamic::instantiate<ui_element>(...)`.
 */
class ui_element : public bison::cloneable_dynamic<ui_element> {
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
  void for_each_child_ordered(const std::function<void(bison::key_t, ui_element&)>& fn) const;

  // ── Layout stash ──────────────────────────────────────────────────────────
  //
  // Native C++ accessors for the transient per-frame geometry described by
  // `layout_stash` above. Every wish widget class's `addClass(...)`
  // registration already passes `dynamic::make_factory<ui_element>(...)` as
  // its factory, so every instantiated widget is already a real
  // `ui_element*` -- this state belongs here, plain and typed, rather than
  // round-tripped through the generic `std::map<key_t,field>` dynamic-field
  // store reserved for genuinely RMI-visible fields. All setters are `const`
  // (writing through the `mutable` `layout_stash_` member below) so they can
  // be called through the same `const ui_element&` every `render_*` function
  // already receives, with no `const_cast` needed.

  /// @brief Node's own natural size, as of the last measure pass.
  vec2f measured_size() const { return layout_stash_.measured; }
  /// @brief Record this frame's measured natural size.
  void set_measured_size(vec2f size) const { layout_stash_.measured = size; }

  /// @brief This node's real, ImGui-computed item rect size from the last
  ///        time it was actually rendered. See `layout_stash::
  ///        last_rendered_size`'s doc comment.
  vec2f last_rendered_size() const { return layout_stash_.last_rendered_size; }
  /// @brief Record this node's real rendered size (called generically for
  ///        every node by `imgui_renderer::render_node()`, not per-class).
  void set_last_rendered_size(vec2f size) const { layout_stash_.last_rendered_size = size; }

  /// @brief Resolved top-left from the last arrange pass, relative to the
  ///        parent's content-region cursor-start origin.
  vec2f arranged_pos() const { return layout_stash_.arranged_pos; }
  /// @brief Resolved content box size from the last arrange pass.
  vec2f arranged_size() const { return layout_stash_.arranged_size; }
  /// @brief Record this frame's resolved arrange geometry.
  /// @param pos    Resolved top-left, relative to the parent's
  ///               content-region cursor-start origin.
  /// @param size   Resolved content box size.
  /// @param frame  `ImGui::GetFrameCount()` at write time; drives
  ///               `is_arrange_fresh()`.
  void set_arranged_rect(vec2f pos, vec2f size, int frame) const {
    layout_stash_.arranged_pos = pos;
    layout_stash_.arranged_size = size;
    layout_stash_.arranged_frame = frame;
  }

  /// @brief True once `set_arranged_rect()` has been called at least once
  ///        (any frame, not just this one) -- distinguishes "never arranged"
  ///        from a legitimate `arranged_pos()`/`arranged_size()` of `{0,0}`.
  bool has_arranged() const { return layout_stash_.arranged_frame >= 0; }

  /// @brief How much space this `VerticalLayout`/`HorizontalLayout` node's
  ///        own children actually consumed after arrangement -- see
  ///        `layout_stash::content_extent`'s doc comment for why this can
  ///        differ from `arranged_size()`. Meaningless (default `{0,0}`) for
  ///        any other class.
  vec2f content_extent() const { return layout_stash_.content_extent; }
  /// @brief Record this frame's actual content extent (see `content_extent()`).
  void set_content_extent(vec2f extent) const { layout_stash_.content_extent = extent; }

  /// @brief True when this node's arrange stash was written during
  ///        @p current_frame.
  ///
  /// Lets `ensure_arranged()` (`src/imgui/imgui_layout.cpp`) distinguish "an
  /// ancestor's top-down pass already resolved this node this frame" from a
  /// stale leftover from a frame where an ancestor `Window`/`TreeNode` was
  /// collapsed or not rendered -- a stale rect must never be trusted.
  bool is_arrange_fresh(int current_frame) const { return layout_stash_.arranged_frame == current_frame; }

 private:
  mutable layout_stash layout_stash_;
};

} // namespace bdg::wish
