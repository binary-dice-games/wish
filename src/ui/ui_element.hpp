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
#include <string>
#include <utility>
#include <vector>

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
  int measured_frame = -1; ///< `ImGui::GetFrameCount()` at the last measure
                            ///< write; -1 means "never measured".
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
                         ///< children stretch to fill it. This -- not
                         ///< `arranged_size` -- is what render_vertical_
                         ///< layout()/render_horizontal_layout() size their
                         ///< own self-managed `BeginChild()` wrap to, so a
                         ///< self-healed row with no stretch child doesn't
                         ///< balloon its own panel out to the whole ambient
                         ///< avail.
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

  // ── Hot field pointer cache ─────────────────────────────────────────────────
  //
  // Lazily-cached `field*` pointers for hot fields read on (nearly) every
  // node in every measure/arrange/render pass. Caches the *pointer* returned
  // by `findField()`, not the field's value: `bison::dynamic` never erases a
  // named-key field once created (only numeric/array-index entries, via
  // `erase(size_t)`/`clear()`), and `fields_` is a node-based `std::map`, so
  // a named field's address is stable for the object's lifetime. Reading
  // through the cached pointer always observes the current value, so this
  // needs no invalidation logic on write, no matter which of `dynamic`'s
  // several independent write paths touched the field. See `CLAUDE.md`'s
  // "Performance: hot-path bison::dynamic field access" section before
  // adding more of these.
  //
  // `cached_field()`/`cached_field_or()` below factor out the boilerplate
  // shared by every accessor: resolve `findField()` at most once (caching
  // whatever it returns, including null -- a field that doesn't exist yet
  // gets a fresh lookup attempt on each call until it does), then either
  // hand back the raw `field*` (for callers with their own is<T>()/emptiness
  // logic, e.g. `profiler_marker()`) or a `T` with a default substituted for
  // "absent or wrong type" (the common case). These are generic across any
  // field, not just the ones cached here -- reuse them for the next hot
  // field found, rather than hand-rolling the pattern again.

  /// @brief Resolve and cache `findField(key)` in `*cache`, reusing it on
  ///        every later call. `*cache` must be a `mutable field*` member
  ///        dedicated to this one field.
  bison::field* cached_field(bison::field*& cache, bison::key_t key) const {
    if (!cache)
      cache = findField(key);
    return cache;
  }

  /// @brief Like `cached_field()`, but returns the field's value as `T`
  ///        (via `field::get_as<T>()`, which cross-type-converts -- e.g. a
  ///        JSON integer literal like `"width": -1` is stored as an
  ///        `int32_t` field, and a `float`-typed accessor must still see
  ///        `-1.0f`, not silently fall back to @p default_value), or
  ///        @p default_value if the field is absent. Matches
  ///        `dynamic::get_as<T>(key, default_value)`'s semantics exactly --
  ///        including throwing for a genuinely incompatible stored type --
  ///        so callers migrating from `get_as()` to a cached accessor see
  ///        no behavior change.
  template <typename T>
  T cached_field_or(bison::field*& cache, bison::key_t key, T default_value) const {
    bison::field* f = cached_field(cache, key);
    return f ? f->get_as<T>() : default_value;
  }

  /// @brief Cached `as<key_t>(dynamic::CLASS)`. CLASS is set exactly once
  ///        per instance, inside `bison::dynamic::instantiate()`, and never
  ///        reassigned afterward in production code.
  bison::key_t class_key() const { return cached_field_or<bison::key_t>(class_field_, dynamic::CLASS, bison::key_t{}); }

  /// @brief Cached `get_as<bool>("visible"_key, true)`.
  bool visible() const { return cached_field_or<bool>(visible_field_, bison::key_t{"visible"}, true); }

  /// @brief Cached `findField("__path__"_key)`'s string value, or `""` if
  ///        absent -- e.g. a form-generated node with no RMI identity of its
  ///        own (see `stable_id()` in `src/imgui/imgui_ui_renderer.cpp`).
  std::string path() const { return cached_field_or<std::string>(path_field_, bison::key_t{"__path__"}, std::string{}); }

  /// @brief Cached `get_as<key_t>("__wish_id"_key, key_t{})`.
  bison::key_t wish_id() const {
    return cached_field_or<bison::key_t>(wish_id_field_, bison::key_t{"__wish_id"}, bison::key_t{});
  }

  /// @brief Cached `get_as<std::string>("font_path"_key, "")`.
  std::string font_path() const {
    return cached_field_or<std::string>(font_path_field_, bison::key_t{"font_path"}, std::string{});
  }

  /// @brief Cached `get_as<float>("font_size"_key, 0.0f)`.
  float font_size() const { return cached_field_or<float>(font_size_field_, bison::key_t{"font_size"}, 0.0f); }

  /// @brief Cached `get_as<bool>("__wish_highlight__"_key, false)`.
  bool highlight() const {
    return cached_field_or<bool>(highlight_field_, bison::key_t{"__wish_highlight__"}, false);
  }

  /// @brief Cached, non-empty `"profiler_marker"` string, or `nullptr` if
  ///        the field is absent, not a string, or empty. Returns a raw
  ///        pointer (rather than going through `cached_field_or()`) because
  ///        callers need "absent" and "present but empty" to both mean "no
  ///        marker", matching `BISON_TRACE_SCOPE`'s `const char*` contract.
  const std::string* profiler_marker() const {
    bison::field* f = cached_field(profiler_marker_field_, bison::key_t{"profiler_marker"});
    return (f && f->is<std::string>() && !f->as<std::string>().empty()) ? &f->as<std::string>() : nullptr;
  }

  /// @brief Cached `get_as<std::string>("label"_key, def)`.
  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }

  /// @brief Cached `get_as<std::string>("text"_key, def)`.
  std::string text(std::string def = {}) const {
    return cached_field_or<std::string>(text_field_, bison::key_t{"text"}, std::move(def));
  }

  /// @brief Cached `get_as<std::string>("hint"_key, def)`.
  std::string hint(std::string def = {}) const {
    return cached_field_or<std::string>(hint_field_, bison::key_t{"hint"}, std::move(def));
  }

  /// @brief Cached `get_as<float>("width"_key, def)`. Most widgets store
  ///        `width` as `float`; use `width_i()` for widgets (window,
  ///        dockspace) that store it as `int32_t` -- both accessors share
  ///        one cached `field*`, since the cache only pins the field's
  ///        location, not its stored type. `def` lets each call site match
  ///        its previous `get_as()` default.
  float width(float def = 0.0f) const { return cached_field_or<float>(width_field_, bison::key_t{"width"}, def); }

  /// @brief Cached `get_as<int32_t>("width"_key, def)`. See `width()`.
  int32_t width_i(int32_t def = 0) const {
    return cached_field_or<int32_t>(width_field_, bison::key_t{"width"}, def);
  }

  /// @brief Cached `get_as<float>("height"_key, def)`. See `width()` for
  ///        why this shares a cache with `height_i()`.
  float height(float def = 0.0f) const { return cached_field_or<float>(height_field_, bison::key_t{"height"}, def); }

  /// @brief Cached `get_as<int32_t>("height"_key, def)`. See `height()`.
  int32_t height_i(int32_t def = 0) const {
    return cached_field_or<int32_t>(height_field_, bison::key_t{"height"}, def);
  }

  /// @brief Cached `get_as<int32_t>("flags"_key, def)`.
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }

  /// @brief Cached `get_as<std::string>("id"_key, def)`.
  std::string id(std::string def = {}) const {
    return cached_field_or<std::string>(id_field_, bison::key_t{"id"}, std::move(def));
  }

  /// @brief Cached `get_as<bool>("value"_key, def)`. `value` is stored as
  ///        a different type per widget (bool/float/int32_t/std::string/
  ///        `std::vector<float>`); all `value_*()` accessors below share one
  ///        cached `field*` -- see `width()`.
  bool value_bool(bool def = false) const { return cached_field_or<bool>(value_field_, bison::key_t{"value"}, def); }

  /// @brief Cached `get_as<float>("value"_key, def)`. See `value_bool()`.
  float value_float(float def = 0.0f) const {
    return cached_field_or<float>(value_field_, bison::key_t{"value"}, def);
  }

  /// @brief Cached `get_as<int32_t>("value"_key, def)`. See `value_bool()`.
  int32_t value_int(int32_t def = 0) const {
    return cached_field_or<int32_t>(value_field_, bison::key_t{"value"}, def);
  }

  /// @brief Cached `get_as<std::string>("value"_key, def)`. See `value_bool()`.
  std::string value_string(std::string def = {}) const {
    return cached_field_or<std::string>(value_field_, bison::key_t{"value"}, std::move(def));
  }

  /// @brief Cached `get_as<std::vector<float>>("value"_key, {})`. See `value_bool()`.
  std::vector<float> value_floats() const {
    return cached_field_or<std::vector<float>>(value_field_, bison::key_t{"value"}, std::vector<float>{});
  }

  /// @brief Cached `get_as<float>("min"_key, def)`. `min` is `float` on
  ///        `*_float` numeric widgets, `int32_t` on `*_int` ones; both
  ///        accessors share one cached `field*` -- see `width()`.
  float min_float(float def = 0.0f) const { return cached_field_or<float>(min_field_, bison::key_t{"min"}, def); }

  /// @brief Cached `get_as<int32_t>("min"_key, def)`. See `min_float()`.
  int32_t min_int(int32_t def = 0) const { return cached_field_or<int32_t>(min_field_, bison::key_t{"min"}, def); }

  /// @brief Cached `get_as<float>("max"_key, def)`. See `min_float()`.
  float max_float(float def = 0.0f) const { return cached_field_or<float>(max_field_, bison::key_t{"max"}, def); }

  /// @brief Cached `get_as<int32_t>("max"_key, def)`. See `min_float()`.
  int32_t max_int(int32_t def = 0) const { return cached_field_or<int32_t>(max_field_, bison::key_t{"max"}, def); }

  /// @brief Cached `get_as<int32_t>("step"_key, def)`. `step` is `int32_t` on
  ///        `*_int` numeric widgets, `float` on `*_float` ones; both
  ///        accessors share one cached `field*` -- see `width()`.
  int32_t step_int(int32_t def = 0) const { return cached_field_or<int32_t>(step_field_, bison::key_t{"step"}, def); }

  /// @brief Cached `get_as<float>("step"_key, def)`. See `step_int()`.
  float step_float(float def = 0.0f) const { return cached_field_or<float>(step_field_, bison::key_t{"step"}, def); }

  /// @brief Cached `get_as<int32_t>("step_fast"_key, def)`. `step_fast` is
  ///        `int32_t` on `*_int` numeric widgets, `float` on `*_float` ones
  ///        (see `step_fast_float()`); both share one cached `field*`.
  int32_t step_fast_int(int32_t def = 0) const {
    return cached_field_or<int32_t>(step_fast_field_, bison::key_t{"step_fast"}, def);
  }

  /// @brief Cached `get_as<float>("step_fast"_key, def)`. See `step_fast_int()`.
  float step_fast_float(float def = 0.0f) const {
    return cached_field_or<float>(step_fast_field_, bison::key_t{"step_fast"}, def);
  }

  /// @brief Cached `get_as<float>("speed"_key, def)`.
  float speed(float def = 0.0f) const { return cached_field_or<float>(speed_field_, bison::key_t{"speed"}, def); }

  /// @brief Cached `get_as<std::string>("format"_key, def)`.
  std::string format(std::string def = {}) const {
    return cached_field_or<std::string>(format_field_, bison::key_t{"format"}, std::move(def));
  }

  /// @brief Cached `get_as<std::string>("orientation"_key, def)`.
  std::string orientation(std::string def = {}) const {
    return cached_field_or<std::string>(orientation_field_, bison::key_t{"orientation"}, std::move(def));
  }

  /// @brief Cached `get_as<float>("thickness"_key, def)`.
  float thickness(float def = 0.0f) const {
    return cached_field_or<float>(thickness_field_, bison::key_t{"thickness"}, def);
  }

  /// @brief Cached `get_as<float>("weight"_key, def)`.
  float weight(float def = 0.0f) const { return cached_field_or<float>(weight_field_, bison::key_t{"weight"}, def); }

  /// @brief Cached `get_as<float>("spacing"_key, def)`.
  float spacing(float def = 0.0f) const {
    return cached_field_or<float>(spacing_field_, bison::key_t{"spacing"}, def);
  }

  /// @brief Cached `get_as<bool>("selected"_key, def)`.
  bool selected(bool def = false) const {
    return cached_field_or<bool>(selected_field_, bison::key_t{"selected"}, def);
  }

  /// @brief Cached `get_as<bool>("enabled"_key, def)`.
  bool enabled(bool def = true) const { return cached_field_or<bool>(enabled_field_, bison::key_t{"enabled"}, def); }

  /// @brief Cached `get_as<bool>("closable"_key, def)`.
  bool closable(bool def = false) const {
    return cached_field_or<bool>(closable_field_, bison::key_t{"closable"}, def);
  }

  /// @brief Cached `get_as<float>("min_height"_key, def)`.
  float min_height(float def = 0.0f) const {
    return cached_field_or<float>(min_height_field_, bison::key_t{"min_height"}, def);
  }

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
  /// @param size   Node's own natural size.
  /// @param frame  `ImGui::GetFrameCount()` at write time; drives
  ///               `is_measure_fresh()`.
  void set_measured_size(vec2f size, int frame) const {
    layout_stash_.measured = size;
    layout_stash_.measured_frame = frame;
  }

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

  /// @brief True when this node's measured-size stash was written during
  ///        @p current_frame.
  ///
  /// Lets `ensure_arranged()` (`src/imgui/imgui_layout.cpp`) skip a
  /// redundant `measure_node()` walk of this subtree when an enclosing
  /// `Window`/modal's own unconditional top-level `measure_node()` call
  /// already primed it earlier this frame -- only the `arrange_node()` call
  /// still needs to run, since (unlike measure) `Window` has no
  /// `arrange_dispatch_fns()` entry and so never arranges descendants
  /// itself. See `src/imgui/DESIGN.md`'s "ensure_arranged() -- the
  /// self-heal mechanism" section.
  bool is_measure_fresh(int current_frame) const { return layout_stash_.measured_frame == current_frame; }

 private:
  mutable bison::field* class_field_ = nullptr;
  mutable bison::field* visible_field_ = nullptr;
  mutable bison::field* path_field_ = nullptr;
  mutable bison::field* wish_id_field_ = nullptr;
  mutable bison::field* font_path_field_ = nullptr;
  mutable bison::field* font_size_field_ = nullptr;
  mutable bison::field* highlight_field_ = nullptr;
  mutable bison::field* profiler_marker_field_ = nullptr;
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* text_field_ = nullptr;
  mutable bison::field* hint_field_ = nullptr;
  mutable bison::field* width_field_ = nullptr;
  mutable bison::field* height_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* id_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* min_field_ = nullptr;
  mutable bison::field* max_field_ = nullptr;
  mutable bison::field* step_field_ = nullptr;
  mutable bison::field* step_fast_field_ = nullptr;
  mutable bison::field* speed_field_ = nullptr;
  mutable bison::field* format_field_ = nullptr;
  mutable bison::field* orientation_field_ = nullptr;
  mutable bison::field* thickness_field_ = nullptr;
  mutable bison::field* weight_field_ = nullptr;
  mutable bison::field* spacing_field_ = nullptr;
  mutable bison::field* selected_field_ = nullptr;
  mutable bison::field* enabled_field_ = nullptr;
  mutable bison::field* closable_field_ = nullptr;
  mutable bison::field* min_height_field_ = nullptr;
  mutable layout_stash layout_stash_;

  // ── for_each_child_ordered() cache ──────────────────────────────────────
  //
  // `children_field_` caches the "children" findField() lookup shared by
  // both `refresh_children_order()` and `for_each_child_ordered()`.
  // `resolved_children_order_` caches the actual (key, ui_element_ptr)
  // pairs in render order, rebuilt by `refresh_children_order()` -- so
  // `for_each_child_ordered()` (called 1-3x per node per frame during
  // measure/arrange/render) iterates it directly with zero further
  // findField() calls, instead of re-resolving every child's field* from
  // "children" via `children->findField(child_key)` on every call. Holds
  // `ui_element_ptr` (not a raw pointer) so each cached child stays alive
  // between refreshes, matching the existing contract that callers re-run
  // `refresh_children_order()` after structurally adding/removing children
  // (see `file_dialog.cpp`, `properties_dialog.cpp`). `__children_order__`
  // is still written to a field for any external/observability consumers,
  // but `for_each_child_ordered()` no longer reads it back.
  mutable bison::field* children_field_ = nullptr;
  mutable std::vector<std::pair<bison::key_t, ui_element_ptr>> resolved_children_order_;
  mutable bool has_resolved_children_order_ = false;
};

} // namespace bdg::wish
