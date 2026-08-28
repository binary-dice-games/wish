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
#include <optional>
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
  ///
  /// Always produces a plain `ui_element`, even for a class registered with a
  /// more-derived factory (e.g. `"TableRow"` -> `ui_table_row`). Prefer
  /// `ui_element_ptr::create()` for runtime-constructed nodes (e.g. table rows
  /// rebuilt from data) so the resulting object matches the C++ type any
  /// render code may `static_cast` it to; this constructor exists for classes
  /// intentionally registered with `dynamic::make_factory<ui_element>()`.
  explicit ui_element_ptr(bison::dynamic&& base);

  /// @brief Instantiate a registered class through its factory (see
  /// `bison::dynamic::create_instance()`), producing the actual registered
  /// C++ type (e.g. `ui_table_row` for `"TableRow"`) rather than a plain
  /// `ui_element`. Use this instead of the `dynamic&&` constructor above for
  /// any class that may have a typed factory -- code that later
  /// `static_cast`s a child to its typed subclass (e.g. `render_table`'s
  /// `static_cast<const ui_table_row&>`) relies on the object actually being
  /// that type, not just carrying the right `CLASS` field value.
  static ui_element_ptr create(bison::key_t ns, bison::key_t klass) {
    return ui_element_ptr{std::static_pointer_cast<ui_element>(
        std::shared_ptr<bison::dynamic>(bison::dynamic::create_instance(ns, klass)))};
  }

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
   * @brief Deep-clone this element, then rebuild its (and every descendant's)
   *        render-order cache.
   *
   * `bison::cloneable_dynamic<ui_element>::clone_ptr()` would otherwise be
   * inherited as-is: it copies fields via `clone_into()` -- recursing into
   * "children" and cloning each descendant's most-derived type -- but
   * `resolved_children_order_`/`has_resolved_children_order_` are plain
   * (non-field) members that `clone_into()` never touches, so a freshly
   * cloned node always starts with `has_resolved_children_order_ = false`.
   * `for_each_child_ordered()` then silently falls back to hash-sorted map
   * order on the entire cloned subtree. This matters in practice because
   * `ui_template::do_instantiate()` (`ui_template.cpp`) builds every
   * template-instantiated tree -- e.g. the whole widget-demo UI -- via
   * exactly this clone path. `clone_into()` clones children bottom-up (a
   * child's `clone_ptr()` returns before its parent's `clone_into()` call
   * returns), so calling `refresh_children_order()` here, after
   * `clone_into()`, is enough to fix this node; each descendant fixes
   * itself the same way as its own `clone_ptr()` unwinds.
   */
  bison::dynamic_ptr clone_ptr() const override {
    bison::dynamic_ptr result{std::make_shared<ui_element>(bison::dynamic{})};
    clone_into(*result);
    static_cast<ui_element&>(*result).refresh_children_order();
    return result;
  }

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

  /// @brief Like `cached_field()`, but never returns null: a field absent
  ///        from this instance and its prototype chain is created (as an
  ///        empty entry) via `operator[]` and cached. For hot *write* paths
  ///        that stamp a field on (nearly) every node every frame -- e.g.
  ///        `report_self_rect()`'s per-frame window-rect fields. `const`
  ///        (writing through a `mutable field*` cache into the `mutable`
  ///        `fields_` map) so it stays callable through the `const
  ///        ui_element&` every `render_*` function receives.
  bison::field* ensured_field(bison::field*& cache, bison::key_t key) const {
    if (!cache)
      cache = &(const_cast<ui_element&>(*this))[key];
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

  /// @brief Like `cached_field_or()`, but for a field storing a
  ///        `std::vector<T>` that hot code needs to iterate without
  ///        copying (e.g. plot series data). Returns `nullptr` if the field
  ///        is absent or not exactly a `std::vector<T>` -- unlike
  ///        `cached_field_or()`, there is no cross-type coercion for
  ///        vectors, matching the pre-refactor `vec_field()`/
  ///        `i32_vec_field()` helpers this replaces.
  template <typename T>
  const std::vector<T>* cached_vector_field(bison::field*& cache, bison::key_t key) const {
    bison::field* f = cached_field(cache, key);
    return (f && f->is<std::vector<T>>()) ? &f->as<std::vector<T>>() : nullptr;
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

  /// @brief Cached `get_as<float>("weight"_key, def)`.
  float weight(float def = 0.0f) const { return cached_field_or<float>(weight_field_, bison::key_t{"weight"}, def); }

  /// @brief Cached `get_as<float>("spacing"_key, def)`.
  float spacing(float def = 0.0f) const {
    return cached_field_or<float>(spacing_field_, bison::key_t{"spacing"}, def);
  }

  // ── Drag & drop ───────────────────────────────────────────────────────────
  //
  // `handle_drag_drop()` (`src/imgui/imgui_renderer.cpp`) is called for every
  // node on every frame, and read `"drag_type"`/`"drag_payload"`/`"drop_type"`
  // off each one -- three `get_as<std::string>()` map lookups per node per
  // frame even though the overwhelming majority of nodes set none of them.
  // Cached like every other hot field.

  /// @brief Cached `get_as<std::string>("drag_type"_key, "")`.
  std::string drag_type() const {
    return cached_field_or<std::string>(drag_type_field_, bison::key_t{"drag_type"}, std::string{});
  }
  /// @brief Cached `get_as<std::string>("drag_payload"_key, "")`.
  std::string drag_payload() const {
    return cached_field_or<std::string>(drag_payload_field_, bison::key_t{"drag_payload"}, std::string{});
  }
  /// @brief Cached `get_as<std::string>("drop_type"_key, "")`.
  std::string drop_type() const {
    return cached_field_or<std::string>(drop_type_field_, bison::key_t{"drop_type"}, std::string{});
  }

  // ── Self-reported window / group rect ─────────────────────────────────────
  //
  // The four classes that open their own real ImGui window or `BeginChild`
  // (Window, DockSpaceViewport, VerticalLayout, HorizontalLayout) stamp their
  // own screen-space rect here every frame via `report_self_rect()`
  // (`src/imgui/imgui_ui_renderer.cpp`); `imgui_renderer::render_node()` reads
  // it straight back the same frame as the authoritative rect for those
  // classes (a `BeginGroup()/EndGroup()` wrap around their dispatch call
  // cannot see across the `Begin()/End()` / `BeginChild()/EndChild()`
  // boundary). Both the write and the read run per frame on every such
  // container, so both go through cached `field*`s -- four named keys that,
  // once created, are never erased, exactly like every other hot field here.

  /// @brief Stamp this frame's self-reported rect (screen-space top-left
  ///        @p pos and @p size). `const` for the same reason as the layout
  ///        stash setters below.
  void set_self_rect(vec2f pos, vec2f size) const {
    *ensured_field(win_rect_x_field_, bison::key_t{"__wish_win_rect_x__"}) = pos.x;
    *ensured_field(win_rect_y_field_, bison::key_t{"__wish_win_rect_y__"}) = pos.y;
    *ensured_field(win_rect_w_field_, bison::key_t{"__wish_win_rect_w__"}) = size.x;
    *ensured_field(win_rect_h_field_, bison::key_t{"__wish_win_rect_h__"}) = size.y;
  }

  /// @brief Read back the rect stamped by `set_self_rect()`. Returns `false`,
  ///        leaving @p pos / @p size untouched, when any component is absent
  ///        or not a `float` -- the expected path for a VerticalLayout/
  ///        HorizontalLayout with a degenerate (<=0) self size that skipped
  ///        its own `BeginChild()`/`set_self_rect()` this frame.
  bool self_rect(vec2f& pos, vec2f& size) const {
    const bison::field* x = cached_field(win_rect_x_field_, bison::key_t{"__wish_win_rect_x__"});
    const bison::field* y = cached_field(win_rect_y_field_, bison::key_t{"__wish_win_rect_y__"});
    const bison::field* w = cached_field(win_rect_w_field_, bison::key_t{"__wish_win_rect_w__"});
    const bison::field* h = cached_field(win_rect_h_field_, bison::key_t{"__wish_win_rect_h__"});
    if (!x || !x->is<float>() || !y || !y->is<float>() || !w || !w->is<float>() || !h || !h->is<float>())
      return false;
    pos = {x->as<float>(), y->as<float>()};
    size = {w->as<float>(), h->as<float>()};
    return true;
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
  mutable bison::field* width_field_ = nullptr;
  mutable bison::field* height_field_ = nullptr;
  mutable bison::field* weight_field_ = nullptr;
  mutable bison::field* spacing_field_ = nullptr;
  mutable bison::field* drag_type_field_ = nullptr;
  mutable bison::field* drag_payload_field_ = nullptr;
  mutable bison::field* drop_type_field_ = nullptr;
  mutable bison::field* win_rect_x_field_ = nullptr;
  mutable bison::field* win_rect_y_field_ = nullptr;
  mutable bison::field* win_rect_w_field_ = nullptr;
  mutable bison::field* win_rect_h_field_ = nullptr;
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

/**
 * @brief CRTP mixin fixing clone-slicing one level below
 *        `bison::cloneable_dynamic<Derived>`.
 *
 * `bison::cloneable_dynamic<ui_element>::clone_ptr()` (inherited by every
 * `ui_element` subclass) hardcodes `std::make_shared<ui_element>(...)` --
 * correct for plain `ui_element` instances, but it slices any further
 * subclass (e.g. `ui_button`) down to base `ui_element` when cloned, which
 * is exactly what `ui_template.cpp`'s prototype instantiation does on every
 * `"instantiate"_key` call. A subsequent `static_cast<const ui_button&>`
 * on the clone (as every per-class `render_*` function now performs) is
 * then undefined behavior.
 *
 * Deriving a leaf class from `cloneable_ui_element<Leaf, Base>` instead of
 * `Base` directly overrides `clone_ptr()` to reconstruct the correct
 * concrete `Leaf` type, while still cloning through `Base`'s (and
 * `dynamic`'s) field map via `clone_into()`.
 *
 * @tparam Derived  The concrete leaf class being defined.
 * @tparam Base     Immediate base class (defaults to `ui_element`; use
 *                   e.g. `ui_root` or `form` for classes deeper in the
 *                   hierarchy).
 */
template <typename Derived, typename Base = ui_element>
class cloneable_ui_element : public Base {
 public:
  explicit cloneable_ui_element(bison::dynamic&& base) : Base(std::move(base)) {}

  bison::dynamic_ptr clone_ptr() const override {
    bison::dynamic_ptr result{std::make_shared<Derived>(bison::dynamic{})};
    this->clone_into(*result);
    // Rebuild the render-order cache on the clone -- see ui_element::
    // clone_ptr()'s doc comment for why this is required after clone_into().
    static_cast<ui_element&>(*result).refresh_children_order();
    return result;
  }
};

// ── Buttons, labels, static text ────────────────────────────────────────────

class ui_button : public cloneable_ui_element<ui_button> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
};

class ui_label : public cloneable_ui_element<ui_label> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string text(std::string def = {}) const {
    return cached_field_or<std::string>(text_field_, bison::key_t{"text"}, std::move(def));
  }
  bool wrap(bool def = false) const { return cached_field_or<bool>(wrap_field_, bison::key_t{"wrap"}, def); }

 private:
  mutable bison::field* text_field_ = nullptr;
  mutable bison::field* wrap_field_ = nullptr;
};

class ui_separator_text : public cloneable_ui_element<ui_separator_text> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
};

// ── Images, layout ───────────────────────────────────────────────────────────

class ui_image : public cloneable_ui_element<ui_image> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string src(std::string def = {}) const {
    return cached_field_or<std::string>(src_field_, bison::key_t{"src"}, std::move(def));
  }
  bool auto_size_to_font(bool def = false) const {
    return cached_field_or<bool>(auto_size_to_font_field_, bison::key_t{"__auto_size_to_font__"}, def);
  }
  bool tint_to_text_color(bool def = false) const {
    return cached_field_or<bool>(tint_to_text_color_field_, bison::key_t{"__tint_to_text_color__"}, def);
  }

 private:
  mutable bison::field* src_field_ = nullptr;
  mutable bison::field* auto_size_to_font_field_ = nullptr;
  mutable bison::field* tint_to_text_color_field_ = nullptr;
};

class ui_horizontal_layout : public cloneable_ui_element<ui_horizontal_layout> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string align(std::string def = {}) const {
    return cached_field_or<std::string>(align_field_, bison::key_t{"align"}, std::move(def));
  }

 private:
  mutable bison::field* align_field_ = nullptr;
};

// ── Checkboxes, radio buttons, selectables ──────────────────────────────────

class ui_checkbox : public cloneable_ui_element<ui_checkbox> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool value_bool(bool def = false) const { return cached_field_or<bool>(value_field_, bison::key_t{"value"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
};

class ui_radio_button : public cloneable_ui_element<ui_radio_button> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool active(bool def = false) const { return cached_field_or<bool>(active_field_, bison::key_t{"active"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* active_field_ = nullptr;
};

class ui_selectable : public cloneable_ui_element<ui_selectable> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool selected(bool def = false) const {
    return cached_field_or<bool>(selected_field_, bison::key_t{"selected"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* selected_field_ = nullptr;
};

// ── Sliders, drags ──────────────────────────────────────────────────────────

class ui_slider_float : public cloneable_ui_element<ui_slider_float> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  float value_float(float def = 0.0f) const {
    return cached_field_or<float>(value_field_, bison::key_t{"value"}, def);
  }
  float min_float(float def = 0.0f) const { return cached_field_or<float>(min_field_, bison::key_t{"min"}, def); }
  float max_float(float def = 0.0f) const { return cached_field_or<float>(max_field_, bison::key_t{"max"}, def); }
  std::string format(std::string def = {}) const {
    return cached_field_or<std::string>(format_field_, bison::key_t{"format"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* min_field_ = nullptr;
  mutable bison::field* max_field_ = nullptr;
  mutable bison::field* format_field_ = nullptr;
};

class ui_slider_int : public cloneable_ui_element<ui_slider_int> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  int32_t value_int(int32_t def = 0) const { return cached_field_or<int32_t>(value_field_, bison::key_t{"value"}, def); }
  int32_t min_int(int32_t def = 0) const { return cached_field_or<int32_t>(min_field_, bison::key_t{"min"}, def); }
  int32_t max_int(int32_t def = 0) const { return cached_field_or<int32_t>(max_field_, bison::key_t{"max"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* min_field_ = nullptr;
  mutable bison::field* max_field_ = nullptr;
};

class ui_drag_float : public cloneable_ui_element<ui_drag_float> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  float value_float(float def = 0.0f) const {
    return cached_field_or<float>(value_field_, bison::key_t{"value"}, def);
  }
  float speed(float def = 0.0f) const { return cached_field_or<float>(speed_field_, bison::key_t{"speed"}, def); }
  float min_float(float def = 0.0f) const { return cached_field_or<float>(min_field_, bison::key_t{"min"}, def); }
  float max_float(float def = 0.0f) const { return cached_field_or<float>(max_field_, bison::key_t{"max"}, def); }
  std::string format(std::string def = {}) const {
    return cached_field_or<std::string>(format_field_, bison::key_t{"format"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* speed_field_ = nullptr;
  mutable bison::field* min_field_ = nullptr;
  mutable bison::field* max_field_ = nullptr;
  mutable bison::field* format_field_ = nullptr;
};

class ui_drag_int : public cloneable_ui_element<ui_drag_int> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  int32_t value_int(int32_t def = 0) const { return cached_field_or<int32_t>(value_field_, bison::key_t{"value"}, def); }
  float speed(float def = 0.0f) const { return cached_field_or<float>(speed_field_, bison::key_t{"speed"}, def); }
  int32_t min_int(int32_t def = 0) const { return cached_field_or<int32_t>(min_field_, bison::key_t{"min"}, def); }
  int32_t max_int(int32_t def = 0) const { return cached_field_or<int32_t>(max_field_, bison::key_t{"max"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* speed_field_ = nullptr;
  mutable bison::field* min_field_ = nullptr;
  mutable bison::field* max_field_ = nullptr;
};

// ── Numeric / text inputs ────────────────────────────────────────────────────

class ui_input_int : public cloneable_ui_element<ui_input_int> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  int32_t value_int(int32_t def = 0) const { return cached_field_or<int32_t>(value_field_, bison::key_t{"value"}, def); }
  int32_t step_int(int32_t def = 0) const { return cached_field_or<int32_t>(step_field_, bison::key_t{"step"}, def); }
  int32_t step_fast_int(int32_t def = 0) const {
    return cached_field_or<int32_t>(step_fast_field_, bison::key_t{"step_fast"}, def);
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* step_field_ = nullptr;
  mutable bison::field* step_fast_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
};

class ui_input_float : public cloneable_ui_element<ui_input_float> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  float value_float(float def = 0.0f) const {
    return cached_field_or<float>(value_field_, bison::key_t{"value"}, def);
  }
  float step_float(float def = 0.0f) const { return cached_field_or<float>(step_field_, bison::key_t{"step"}, def); }
  float step_fast_float(float def = 0.0f) const {
    return cached_field_or<float>(step_fast_field_, bison::key_t{"step_fast"}, def);
  }
  std::string format(std::string def = {}) const {
    return cached_field_or<std::string>(format_field_, bison::key_t{"format"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* step_field_ = nullptr;
  mutable bison::field* step_fast_field_ = nullptr;
  mutable bison::field* format_field_ = nullptr;
};

class ui_input_text : public cloneable_ui_element<ui_input_text> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  std::string hint(std::string def = {}) const {
    return cached_field_or<std::string>(hint_field_, bison::key_t{"hint"}, std::move(def));
  }
  std::string value_string(std::string def = {}) const {
    return cached_field_or<std::string>(value_field_, bison::key_t{"value"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  bool multiline(bool def = false) const {
    return cached_field_or<bool>(multiline_field_, bison::key_t{"multiline"}, def);
  }
  int32_t max_length(int32_t def = 256) const {
    return cached_field_or<int32_t>(max_length_field_, bison::key_t{"max_length"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* hint_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* multiline_field_ = nullptr;
  mutable bison::field* max_length_field_ = nullptr;
};

class ui_color_edit : public cloneable_ui_element<ui_color_edit> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  std::vector<float> value_floats() const {
    return cached_field_or<std::vector<float>>(value_field_, bison::key_t{"value"}, std::vector<float>{});
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
};

class ui_combo : public cloneable_ui_element<ui_combo> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  int32_t value_int(int32_t def = 0) const { return cached_field_or<int32_t>(value_field_, bison::key_t{"value"}, def); }
  std::string items(std::string def = {}) const {
    return cached_field_or<std::string>(items_field_, bison::key_t{"items"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
  mutable bison::field* items_field_ = nullptr;
};

class ui_progress_bar : public cloneable_ui_element<ui_progress_bar> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  float value_float(float def = 0.0f) const {
    return cached_field_or<float>(value_field_, bison::key_t{"value"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* value_field_ = nullptr;
};

// ── Menus ────────────────────────────────────────────────────────────────────

class ui_menu : public cloneable_ui_element<ui_menu> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool enabled(bool def = true) const { return cached_field_or<bool>(enabled_field_, bison::key_t{"enabled"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* enabled_field_ = nullptr;
};

class ui_menu_item : public cloneable_ui_element<ui_menu_item> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool enabled(bool def = true) const { return cached_field_or<bool>(enabled_field_, bison::key_t{"enabled"}, def); }
  std::string shortcut(std::string def = {}) const {
    return cached_field_or<std::string>(shortcut_field_, bison::key_t{"shortcut"}, std::move(def));
  }
  bool checked(bool def = false) const { return cached_field_or<bool>(checked_field_, bison::key_t{"checked"}, def); }
  std::string copy_text(std::string def = {}) const {
    return cached_field_or<std::string>(copy_text_field_, bison::key_t{"copy_text"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* enabled_field_ = nullptr;
  mutable bison::field* shortcut_field_ = nullptr;
  mutable bison::field* checked_field_ = nullptr;
  mutable bison::field* copy_text_field_ = nullptr;
};

class ui_menu_button : public cloneable_ui_element<ui_menu_button> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }

 private:
  mutable bison::field* label_field_ = nullptr;
};

// ── Tabs ─────────────────────────────────────────────────────────────────────

class ui_tab_item : public cloneable_ui_element<ui_tab_item> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool closable(bool def = false) const {
    return cached_field_or<bool>(closable_field_, bison::key_t{"closable"}, def);
  }
  bool scroll(bool def = false) const {
    return cached_field_or<bool>(scroll_field_, bison::key_t{"scroll"}, def);
  }

  /// @brief True iff @p is_selected differs from the selected state
  /// recorded on the previous call (or this is the first call); always
  /// updates the recorded state. Replaces `"__selected__"`.
  bool toggled_since_last_frame(bool is_selected) const {
    bool was = was_selected_.value_or(is_selected);
    was_selected_ = is_selected;
    return was != is_selected;
  }

  /// @brief Last selected state recorded by toggled_since_last_frame(),
  /// without mutating it. false before the first render.
  bool is_selected() const { return was_selected_.value_or(false); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* closable_field_ = nullptr;
  mutable bison::field* scroll_field_ = nullptr;
  mutable std::optional<bool> was_selected_;
};

class ui_tab_bar : public cloneable_ui_element<ui_tab_bar> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string id(std::string def = {}) const {
    return cached_field_or<std::string>(id_field_, bison::key_t{"id"}, std::move(def));
  }

 private:
  mutable bison::field* id_field_ = nullptr;
};

// ── Splitter ─────────────────────────────────────────────────────────────────

class ui_splitter : public cloneable_ui_element<ui_splitter> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string orientation(std::string def = {}) const {
    return cached_field_or<std::string>(orientation_field_, bison::key_t{"orientation"}, std::move(def));
  }
  float thickness(float def = 0.0f) const {
    return cached_field_or<float>(thickness_field_, bison::key_t{"thickness"}, def);
  }
  float min_pane_size(float def = 20.0f) const {
    return cached_field_or<float>(min_pane_size_field_, bison::key_t{"min_pane_size"}, def);
  }

  /// @brief True the first time this is called, false on every later call.
  /// Replaces `"__splitter_inited__"` -- pure per-frame render bookkeeping
  /// never read outside render_splitter(), so it doesn't need to be a
  /// bison::dynamic field.
  bool consume_init_guard() const {
    if (inited_)
      return false;
    inited_ = true;
    return true;
  }

 private:
  mutable bison::field* orientation_field_ = nullptr;
  mutable bison::field* thickness_field_ = nullptr;
  mutable bison::field* min_pane_size_field_ = nullptr;
  mutable bool inited_ = false;
};

// ── Tree ─────────────────────────────────────────────────────────────────────

class ui_tree_node : public cloneable_ui_element<ui_tree_node> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  bool open(bool def = false) const { return cached_field_or<bool>(open_field_, bison::key_t{"open"}, def); }
  bool leaf(bool def = false) const { return cached_field_or<bool>(leaf_field_, bison::key_t{"leaf"}, def); }

  /// @brief True iff @p is_open differs from the open state recorded on the
  /// previous call (or this is the first call); always updates the
  /// recorded state. Replaces `"__open__"`.
  bool toggled_since_last_frame(bool is_open) const {
    bool was = was_open_.value_or(is_open);
    was_open_ = is_open;
    return was != is_open;
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* open_field_ = nullptr;
  mutable bison::field* leaf_field_ = nullptr;
  mutable std::optional<bool> was_open_;
};

class ui_collapsing_header : public cloneable_ui_element<ui_collapsing_header> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }

  /// @brief True iff @p is_open differs from the open state recorded on the
  /// previous call (or this is the first call); always updates the
  /// recorded state. Replaces `"__open__"`.
  bool toggled_since_last_frame(bool is_open) const {
    bool was = was_open_.value_or(is_open);
    was_open_ = is_open;
    return was != is_open;
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable std::optional<bool> was_open_;
};

// ── Docking, table ───────────────────────────────────────────────────────────

class ui_dockspace_viewport : public cloneable_ui_element<ui_dockspace_viewport> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string id(std::string def = {}) const {
    return cached_field_or<std::string>(id_field_, bison::key_t{"id"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  bool passthru(bool def = false) const {
    return cached_field_or<bool>(passthru_field_, bison::key_t{"passthru"}, def);
  }

 private:
  mutable bison::field* id_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* passthru_field_ = nullptr;
};

class ui_dockspace : public cloneable_ui_element<ui_dockspace> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string id(std::string def = {}) const {
    return cached_field_or<std::string>(id_field_, bison::key_t{"id"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }

 private:
  mutable bison::field* id_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
};

class ui_table : public cloneable_ui_element<ui_table> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string id(std::string def = {}) const {
    return cached_field_or<std::string>(id_field_, bison::key_t{"id"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  int32_t columns(int32_t def = 1) const { return cached_field_or<int32_t>(columns_field_, bison::key_t{"columns"}, def); }
  float outer_width(float def = 0.0f) const {
    return cached_field_or<float>(outer_width_field_, bison::key_t{"outer_width"}, def);
  }
  float outer_height(float def = 0.0f) const {
    return cached_field_or<float>(outer_height_field_, bison::key_t{"outer_height"}, def);
  }
  float inner_width(float def = 0.0f) const {
    return cached_field_or<float>(inner_width_field_, bison::key_t{"inner_width"}, def);
  }
  bool headers(bool def = false) const { return cached_field_or<bool>(headers_field_, bison::key_t{"headers"}, def); }
  bool auto_scroll(bool def = true) const {
    return cached_field_or<bool>(auto_scroll_field_, bison::key_t{"auto_scroll"}, def);
  }

 private:
  mutable bison::field* id_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* columns_field_ = nullptr;
  mutable bison::field* outer_width_field_ = nullptr;
  mutable bison::field* outer_height_field_ = nullptr;
  mutable bison::field* inner_width_field_ = nullptr;
  mutable bison::field* headers_field_ = nullptr;
  mutable bison::field* auto_scroll_field_ = nullptr;
};

class ui_table_row : public cloneable_ui_element<ui_table_row> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  float min_height(float def = 0.0f) const {
    return cached_field_or<float>(min_height_field_, bison::key_t{"min_height"}, def);
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  bool selected(bool def = false) const {
    return cached_field_or<bool>(selected_field_, bison::key_t{"selected"}, def);
  }

 private:
  mutable bison::field* min_height_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* selected_field_ = nullptr;
};

/// @brief TableColumn: read generically from render_table's column-setup
/// loop only after it has already narrowed to class_key() ==
/// "TableColumn"_key, so it casts rather than keeping these on base.
class ui_table_column : public cloneable_ui_element<ui_table_column> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  float init_width(float def = 0.0f) const {
    return cached_field_or<float>(init_width_field_, bison::key_t{"init_width"}, def);
  }
  int32_t column_id(int32_t def = 0) const {
    return cached_field_or<int32_t>(column_id_field_, bison::key_t{"column_id"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* init_width_field_ = nullptr;
  mutable bison::field* column_id_field_ = nullptr;
};

// ── Graph node ───────────────────────────────────────────────────────────────

class ui_graph_node : public cloneable_ui_element<ui_graph_node> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  int32_t lane(int32_t def = 0) const { return cached_field_or<int32_t>(lane_field_, bison::key_t{"lane"}, def); }
  int32_t color(int32_t def = 0) const { return cached_field_or<int32_t>(color_field_, bison::key_t{"color"}, def); }
  bool is_head(bool def = false) const { return cached_field_or<bool>(is_head_field_, bison::key_t{"is_head"}, def); }
  bool is_working(bool def = false) const {
    return cached_field_or<bool>(is_working_field_, bison::key_t{"is_working"}, def);
  }
  float lane_width(float def = 16.0f) const {
    return cached_field_or<float>(lane_width_field_, bison::key_t{"lane_width"}, def);
  }
  float dot_radius(float def = 4.5f) const {
    return cached_field_or<float>(dot_radius_field_, bison::key_t{"dot_radius"}, def);
  }
  float row_height(float def = 0.0f) const {
    return cached_field_or<float>(row_height_field_, bison::key_t{"row_height"}, def);
  }
  const std::vector<int32_t>* top_from() const {
    return cached_vector_field<int32_t>(top_from_field_, bison::key_t{"top_from"});
  }
  const std::vector<int32_t>* top_to() const {
    return cached_vector_field<int32_t>(top_to_field_, bison::key_t{"top_to"});
  }
  const std::vector<int32_t>* top_color() const {
    return cached_vector_field<int32_t>(top_color_field_, bison::key_t{"top_color"});
  }
  const std::vector<int32_t>* bottom_from() const {
    return cached_vector_field<int32_t>(bottom_from_field_, bison::key_t{"bottom_from"});
  }
  const std::vector<int32_t>* bottom_to() const {
    return cached_vector_field<int32_t>(bottom_to_field_, bison::key_t{"bottom_to"});
  }
  const std::vector<int32_t>* bottom_color() const {
    return cached_vector_field<int32_t>(bottom_color_field_, bison::key_t{"bottom_color"});
  }

 private:
  mutable bison::field* lane_field_ = nullptr;
  mutable bison::field* color_field_ = nullptr;
  mutable bison::field* is_head_field_ = nullptr;
  mutable bison::field* is_working_field_ = nullptr;
  mutable bison::field* lane_width_field_ = nullptr;
  mutable bison::field* dot_radius_field_ = nullptr;
  mutable bison::field* row_height_field_ = nullptr;
  mutable bison::field* top_from_field_ = nullptr;
  mutable bison::field* top_to_field_ = nullptr;
  mutable bison::field* top_color_field_ = nullptr;
  mutable bison::field* bottom_from_field_ = nullptr;
  mutable bison::field* bottom_to_field_ = nullptr;
  mutable bison::field* bottom_color_field_ = nullptr;
};

// ── Plot (ImPlot) ────────────────────────────────────────────────────────────
//
// PlotItem, the hidden schema base class every concrete series class
// inherits from at the `bison::dynamic` class-registration level, is never
// itself instantiated/rendered, so it keeps the plain `ui_element` factory
// registered for it and has no dedicated C++ type here.
//
// Several sibling series classes (PlotLine/PlotScatter/PlotStairs/
// PlotDigital; PlotBars/PlotBarsH; Plot3DLine/Plot3DScatter/Plot3DTriangle/
// Plot3DQuad below) share an identical field shape and accessor set, so one
// C++ type is registered as the factory for each of those sibling classes
// rather than duplicating an otherwise-identical class body per class key --
// `class_key()` (already cached on the base) is what a render function uses
// to tell them apart, not the C++ type.

class ui_plot : public cloneable_ui_element<ui_plot> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string title(std::string def = {}) const {
    return cached_field_or<std::string>(title_field_, bison::key_t{"title"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  std::string x_label(std::string def = {}) const {
    return cached_field_or<std::string>(x_label_field_, bison::key_t{"x_label"}, std::move(def));
  }
  std::string y_label(std::string def = {}) const {
    return cached_field_or<std::string>(y_label_field_, bison::key_t{"y_label"}, std::move(def));
  }
  int32_t x_flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(x_flags_field_, bison::key_t{"x_flags"}, def);
  }
  int32_t y_flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(y_flags_field_, bison::key_t{"y_flags"}, def);
  }
  float x_min(float def = 0.0f) const { return cached_field_or<float>(x_min_field_, bison::key_t{"x_min"}, def); }
  float x_max(float def = 0.0f) const { return cached_field_or<float>(x_max_field_, bison::key_t{"x_max"}, def); }
  float y_min(float def = 0.0f) const { return cached_field_or<float>(y_min_field_, bison::key_t{"y_min"}, def); }
  float y_max(float def = 0.0f) const { return cached_field_or<float>(y_max_field_, bison::key_t{"y_max"}, def); }

 private:
  mutable bison::field* title_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* x_label_field_ = nullptr;
  mutable bison::field* y_label_field_ = nullptr;
  mutable bison::field* x_flags_field_ = nullptr;
  mutable bison::field* y_flags_field_ = nullptr;
  mutable bison::field* x_min_field_ = nullptr;
  mutable bison::field* x_max_field_ = nullptr;
  mutable bison::field* y_min_field_ = nullptr;
  mutable bison::field* y_max_field_ = nullptr;
};

/// @brief Shared C++ type for PlotLine, PlotScatter, PlotStairs, and
/// PlotDigital -- all four are `label` + `xs` + `ys`, differing only in
/// which ImPlot::PlotXxx() call the renderer dispatches to by `class_key()`.
class ui_plot_xy_series : public cloneable_ui_element<ui_plot_xy_series> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
};

class ui_plot_stems : public cloneable_ui_element<ui_plot_stems> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  float ref(float def = 0.0f) const { return cached_field_or<float>(ref_field_, bison::key_t{"ref"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* ref_field_ = nullptr;
};

class ui_plot_shaded : public cloneable_ui_element<ui_plot_shaded> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  const std::vector<float>* ys2() const { return cached_vector_field<float>(ys2_field_, bison::key_t{"ys2"}); }
  float ref(float def = 0.0f) const { return cached_field_or<float>(ref_field_, bison::key_t{"ref"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* ys2_field_ = nullptr;
  mutable bison::field* ref_field_ = nullptr;
};

/// @brief Shared C++ type for PlotBars and PlotBarsH -- identical fields;
/// only the ImPlotBarsFlags_Horizontal dispatch differs by `class_key()`.
class ui_plot_bars : public cloneable_ui_element<ui_plot_bars> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  float bar_size(float def = 0.67f) const {
    return cached_field_or<float>(bar_size_field_, bison::key_t{"bar_size"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* bar_size_field_ = nullptr;
};

class ui_plot_histogram : public cloneable_ui_element<ui_plot_histogram> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* values() const {
    return cached_vector_field<float>(values_field_, bison::key_t{"values"});
  }
  int32_t bins(int32_t def = -1) const { return cached_field_or<int32_t>(bins_field_, bison::key_t{"bins"}, def); }
  bool cumulative(bool def = false) const {
    return cached_field_or<bool>(cumulative_field_, bison::key_t{"cumulative"}, def);
  }
  bool density(bool def = false) const {
    return cached_field_or<bool>(density_field_, bison::key_t{"density"}, def);
  }
  float range_min(float def = 0.0f) const {
    return cached_field_or<float>(range_min_field_, bison::key_t{"range_min"}, def);
  }
  float range_max(float def = 0.0f) const {
    return cached_field_or<float>(range_max_field_, bison::key_t{"range_max"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* values_field_ = nullptr;
  mutable bison::field* bins_field_ = nullptr;
  mutable bison::field* cumulative_field_ = nullptr;
  mutable bison::field* density_field_ = nullptr;
  mutable bison::field* range_min_field_ = nullptr;
  mutable bison::field* range_max_field_ = nullptr;
};

class ui_plot_histogram2d : public cloneable_ui_element<ui_plot_histogram2d> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  int32_t x_bins(int32_t def = -1) const {
    return cached_field_or<int32_t>(x_bins_field_, bison::key_t{"x_bins"}, def);
  }
  int32_t y_bins(int32_t def = -1) const {
    return cached_field_or<int32_t>(y_bins_field_, bison::key_t{"y_bins"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* x_bins_field_ = nullptr;
  mutable bison::field* y_bins_field_ = nullptr;
};

class ui_plot_heatmap : public cloneable_ui_element<ui_plot_heatmap> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* values() const {
    return cached_vector_field<float>(values_field_, bison::key_t{"values"});
  }
  int32_t rows(int32_t def = 1) const { return cached_field_or<int32_t>(rows_field_, bison::key_t{"rows"}, def); }
  int32_t cols(int32_t def = 1) const { return cached_field_or<int32_t>(cols_field_, bison::key_t{"cols"}, def); }
  float scale_min(float def = 0.0f) const {
    return cached_field_or<float>(scale_min_field_, bison::key_t{"scale_min"}, def);
  }
  float scale_max(float def = 1.0f) const {
    return cached_field_or<float>(scale_max_field_, bison::key_t{"scale_max"}, def);
  }
  std::string format(std::string def = {}) const {
    return cached_field_or<std::string>(format_field_, bison::key_t{"format"}, std::move(def));
  }
  float x_min(float def = 0.0f) const { return cached_field_or<float>(x_min_field_, bison::key_t{"x_min"}, def); }
  float x_max(float def = 1.0f) const { return cached_field_or<float>(x_max_field_, bison::key_t{"x_max"}, def); }
  float y_min(float def = 0.0f) const { return cached_field_or<float>(y_min_field_, bison::key_t{"y_min"}, def); }
  float y_max(float def = 1.0f) const { return cached_field_or<float>(y_max_field_, bison::key_t{"y_max"}, def); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* values_field_ = nullptr;
  mutable bison::field* rows_field_ = nullptr;
  mutable bison::field* cols_field_ = nullptr;
  mutable bison::field* scale_min_field_ = nullptr;
  mutable bison::field* scale_max_field_ = nullptr;
  mutable bison::field* format_field_ = nullptr;
  mutable bison::field* x_min_field_ = nullptr;
  mutable bison::field* x_max_field_ = nullptr;
  mutable bison::field* y_min_field_ = nullptr;
  mutable bison::field* y_max_field_ = nullptr;
};

class ui_plot_pie_chart : public cloneable_ui_element<ui_plot_pie_chart> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string labels(std::string def = {}) const {
    return cached_field_or<std::string>(labels_field_, bison::key_t{"labels"}, std::move(def));
  }
  const std::vector<float>* values() const {
    return cached_vector_field<float>(values_field_, bison::key_t{"values"});
  }
  float x(float def = 0.5f) const { return cached_field_or<float>(x_field_, bison::key_t{"x"}, def); }
  float y(float def = 0.5f) const { return cached_field_or<float>(y_field_, bison::key_t{"y"}, def); }
  float radius(float def = 0.4f) const {
    return cached_field_or<float>(radius_field_, bison::key_t{"radius"}, def);
  }
  bool normalize(bool def = false) const {
    return cached_field_or<bool>(normalize_field_, bison::key_t{"normalize"}, def);
  }
  std::string label_fmt(std::string def = {}) const {
    return cached_field_or<std::string>(label_fmt_field_, bison::key_t{"label_fmt"}, std::move(def));
  }
  float angle0(float def = 90.0f) const {
    return cached_field_or<float>(angle0_field_, bison::key_t{"angle0"}, def);
  }

 private:
  mutable bison::field* labels_field_ = nullptr;
  mutable bison::field* values_field_ = nullptr;
  mutable bison::field* x_field_ = nullptr;
  mutable bison::field* y_field_ = nullptr;
  mutable bison::field* radius_field_ = nullptr;
  mutable bison::field* normalize_field_ = nullptr;
  mutable bison::field* label_fmt_field_ = nullptr;
  mutable bison::field* angle0_field_ = nullptr;
};

class ui_plot_text : public cloneable_ui_element<ui_plot_text> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string text(std::string def = {}) const {
    return cached_field_or<std::string>(text_field_, bison::key_t{"text"}, std::move(def));
  }
  float x(float def = 0.0f) const { return cached_field_or<float>(x_field_, bison::key_t{"x"}, def); }
  float y(float def = 0.0f) const { return cached_field_or<float>(y_field_, bison::key_t{"y"}, def); }
  float offset_x(float def = 0.0f) const {
    return cached_field_or<float>(offset_x_field_, bison::key_t{"offset_x"}, def);
  }
  float offset_y(float def = 0.0f) const {
    return cached_field_or<float>(offset_y_field_, bison::key_t{"offset_y"}, def);
  }

 private:
  mutable bison::field* text_field_ = nullptr;
  mutable bison::field* x_field_ = nullptr;
  mutable bison::field* y_field_ = nullptr;
  mutable bison::field* offset_x_field_ = nullptr;
  mutable bison::field* offset_y_field_ = nullptr;
};

class ui_plot_inf_lines : public cloneable_ui_element<ui_plot_inf_lines> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* values() const {
    return cached_vector_field<float>(values_field_, bison::key_t{"values"});
  }
  bool horizontal(bool def = false) const {
    return cached_field_or<bool>(horizontal_field_, bison::key_t{"horizontal"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* values_field_ = nullptr;
  mutable bison::field* horizontal_field_ = nullptr;
};

// ── Plot3D (ImPlot3D) ────────────────────────────────────────────────────────
//
// Plot3DItem, like PlotItem above, is a hidden schema base never itself
// instantiated, so it keeps the plain `ui_element` factory.

class ui_plot3d : public cloneable_ui_element<ui_plot3d> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string title(std::string def = {}) const {
    return cached_field_or<std::string>(title_field_, bison::key_t{"title"}, std::move(def));
  }
  std::string x_label(std::string def = {}) const {
    return cached_field_or<std::string>(x_label_field_, bison::key_t{"x_label"}, std::move(def));
  }
  std::string y_label(std::string def = {}) const {
    return cached_field_or<std::string>(y_label_field_, bison::key_t{"y_label"}, std::move(def));
  }
  std::string z_label(std::string def = {}) const {
    return cached_field_or<std::string>(z_label_field_, bison::key_t{"z_label"}, std::move(def));
  }
  int32_t flags(int32_t def = 0) const { return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def); }
  int32_t x_flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(x_flags_field_, bison::key_t{"x_flags"}, def);
  }
  int32_t y_flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(y_flags_field_, bison::key_t{"y_flags"}, def);
  }
  int32_t z_flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(z_flags_field_, bison::key_t{"z_flags"}, def);
  }

 private:
  mutable bison::field* title_field_ = nullptr;
  mutable bison::field* x_label_field_ = nullptr;
  mutable bison::field* y_label_field_ = nullptr;
  mutable bison::field* z_label_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* x_flags_field_ = nullptr;
  mutable bison::field* y_flags_field_ = nullptr;
  mutable bison::field* z_flags_field_ = nullptr;
};

/// @brief Shared C++ type for Plot3DLine, Plot3DScatter, Plot3DTriangle, and
/// Plot3DQuad -- all four are `label` + `xs` + `ys` + `zs`, differing only
/// in which ImPlot3D::PlotXxx() call the renderer dispatches to.
class ui_plot3d_xyz_series : public cloneable_ui_element<ui_plot3d_xyz_series> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  const std::vector<float>* zs() const { return cached_vector_field<float>(zs_field_, bison::key_t{"zs"}); }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* zs_field_ = nullptr;
};

class ui_plot3d_surface : public cloneable_ui_element<ui_plot3d_surface> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  const std::vector<float>* zs() const { return cached_vector_field<float>(zs_field_, bison::key_t{"zs"}); }
  int32_t x_count(int32_t def = 2) const {
    return cached_field_or<int32_t>(x_count_field_, bison::key_t{"x_count"}, def);
  }
  int32_t y_count(int32_t def = 2) const {
    return cached_field_or<int32_t>(y_count_field_, bison::key_t{"y_count"}, def);
  }
  float scale_min(float def = 0.0f) const {
    return cached_field_or<float>(scale_min_field_, bison::key_t{"scale_min"}, def);
  }
  float scale_max(float def = 0.0f) const {
    return cached_field_or<float>(scale_max_field_, bison::key_t{"scale_max"}, def);
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* zs_field_ = nullptr;
  mutable bison::field* x_count_field_ = nullptr;
  mutable bison::field* y_count_field_ = nullptr;
  mutable bison::field* scale_min_field_ = nullptr;
  mutable bison::field* scale_max_field_ = nullptr;
};

class ui_plot3d_mesh : public cloneable_ui_element<ui_plot3d_mesh> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string label(std::string def = {}) const {
    return cached_field_or<std::string>(label_field_, bison::key_t{"label"}, std::move(def));
  }
  const std::vector<float>* xs() const { return cached_vector_field<float>(xs_field_, bison::key_t{"xs"}); }
  const std::vector<float>* ys() const { return cached_vector_field<float>(ys_field_, bison::key_t{"ys"}); }
  const std::vector<float>* zs() const { return cached_vector_field<float>(zs_field_, bison::key_t{"zs"}); }
  const std::vector<int32_t>* indices() const {
    return cached_vector_field<int32_t>(indices_field_, bison::key_t{"indices"});
  }

 private:
  mutable bison::field* label_field_ = nullptr;
  mutable bison::field* xs_field_ = nullptr;
  mutable bison::field* ys_field_ = nullptr;
  mutable bison::field* zs_field_ = nullptr;
  mutable bison::field* indices_field_ = nullptr;
};

class ui_plot3d_text : public cloneable_ui_element<ui_plot3d_text> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string text(std::string def = {}) const {
    return cached_field_or<std::string>(text_field_, bison::key_t{"text"}, std::move(def));
  }
  float x(float def = 0.0f) const { return cached_field_or<float>(x_field_, bison::key_t{"x"}, def); }
  float y(float def = 0.0f) const { return cached_field_or<float>(y_field_, bison::key_t{"y"}, def); }
  float z(float def = 0.0f) const { return cached_field_or<float>(z_field_, bison::key_t{"z"}, def); }
  float angle(float def = 0.0f) const {
    return cached_field_or<float>(angle_field_, bison::key_t{"angle"}, def);
  }
  float offset_x(float def = 0.0f) const {
    return cached_field_or<float>(offset_x_field_, bison::key_t{"offset_x"}, def);
  }
  float offset_y(float def = 0.0f) const {
    return cached_field_or<float>(offset_y_field_, bison::key_t{"offset_y"}, def);
  }

 private:
  mutable bison::field* text_field_ = nullptr;
  mutable bison::field* x_field_ = nullptr;
  mutable bison::field* y_field_ = nullptr;
  mutable bison::field* z_field_ = nullptr;
  mutable bison::field* angle_field_ = nullptr;
  mutable bison::field* offset_x_field_ = nullptr;
  mutable bison::field* offset_y_field_ = nullptr;
};

// ── Text editor ──────────────────────────────────────────────────────────────

class ui_text_editor : public cloneable_ui_element<ui_text_editor> {
 public:
  using cloneable_ui_element::cloneable_ui_element;

  std::string file_path(std::string def = {}) const {
    return cached_field_or<std::string>(file_path_field_, bison::key_t{"file_path"}, std::move(def));
  }
  std::string language(std::string def = {}) const {
    return cached_field_or<std::string>(language_field_, bison::key_t{"language"}, std::move(def));
  }
  bool read_only(bool def = false) const {
    return cached_field_or<bool>(read_only_field_, bison::key_t{"read_only"}, def);
  }
  bool wish_ui_schema(bool def = false) const {
    return cached_field_or<bool>(wish_ui_schema_field_, bison::key_t{"wish_ui_schema"}, def);
  }

 private:
  mutable bison::field* file_path_field_ = nullptr;
  mutable bison::field* language_field_ = nullptr;
  mutable bison::field* read_only_field_ = nullptr;
  mutable bison::field* wish_ui_schema_field_ = nullptr;
};

} // namespace bdg::wish
