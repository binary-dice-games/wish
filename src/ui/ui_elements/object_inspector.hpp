// MIT License © 2025 Binary Dice Games
/**
 * @file object_inspector.hpp
 * @brief `object_inspector` -- a `ui_element` that reflects over a bison
 *        `dynamic` object's registered class to display/edit its fields in
 *        a two-column table (Name | Value) plus a description panel below,
 *        registered as `"ObjectInspector"` in the `"wish"` bison namespace.
 */
#pragma once

#include <context/context.hpp>
#include <ui/ui_element.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/server/context.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/**
 * @brief A Unity/Visual-Studio-style property inspector: given a `"target"`
 *        object, reflects over its registered class (walking the full
 *        `PARENT` chain) to build one table row per visible field --
 *        name + a type/attribute-appropriate editable widget -- and a
 *        description panel below showing the selected row's field
 *        description.
 *
 * Unlike a plain leaf element (`Checkbox`, `Table`, ...), whose behavior
 * lives entirely in its render function, `object_inspector` needs to
 * *construct real child elements* (a `Table` of rows, each row's value
 * widget) from a live target object -- something no render function may do,
 * since the render loop only holds each session's *read* lock (see
 * `wish/CLAUDE.md`'s "Session threading model"; structural tree mutation
 * always happens under the write lock elsewhere in this codebase, e.g.
 * `ui_template::instantiate_prototype()`, `message_box::rebuild()`).
 * `object_inspector` follows that same precedent: its children are built by
 * `set_target()`, called either from the `"__construct"`/`"set_target"`
 * RMI methods below (always dispatched under the write lock) or directly by
 * server-side C++ code that already holds it (mirrors
 * `stamp_widget()`-style direct construction used elsewhere in this
 * codebase for server-authored trees) -- never from render.
 *
 * Once built, `object_inspector` renders exactly like a `VerticalLayout`
 * (see `register_object_inspector()`'s render-function registration) --
 * its own class exists purely for identity in `get_tree()`/automation/docs,
 * not for any bespoke drawing.
 *
 * ### Reflection is process-local
 *
 * Bison attributes (`DisplayName`, `Description`, `Range`, ...) are C++
 * metadata, never serialized over RMI (see `bison::attribute`'s doc
 * comment). This widget can therefore only produce a meaningful table when
 * `target`'s class is registered in the *same process* that renders it --
 * always true for an in-process/standalone app (e.g. genie), not
 * guaranteed for an arbitrary split wish client/server deployment unless
 * the client's domain classes are also registered server-side.
 *
 * ### Field -> widget dispatch
 *
 * In priority order (a field is skipped entirely if it carries a `Hidden`
 * or `Obsolete` attribute, or is one of `CLASS`/`PARENT`/`NAMESPACE`; the
 * visible set is then sorted ascending by `Order` where present, falling
 * back to declaration order):
 *
 *   - `bool` -> `Checkbox`
 *   - `int32_t` + `Enum` -> `Combo` (items = `Enum::entries()`; commits the
 *     selected entry's name string, using `Enum`'s existing string coercion)
 *   - `int32_t` + `EnumFlags` -> `InputText` (`"FlagA | FlagB"` syntax, via
 *     `EnumFlags::format`/`parse`, already built into `field::get_as`/`operator=`)
 *   - `int32_t` + `Range` -> `SliderInt`; else `InputInt`
 *   - `float` + `Range` -> `SliderFloat`; else `InputFloat`
 *   - `std::string` + `Multiline` -> `InputText` with `multiline=true`; else `InputText`
 *   - `std::vector<float>` + `ColorField` -> `ColorEdit`; else a
 *     comma-separated `InputText` (component count preserved on commit)
 *   - `dynamic_ptr` -> read-only reference `Button`; if `DropTarget` is
 *     present, also a drop target (`handle_dropped()` surfaces the drop)
 *   - anything else (`key_t`/`hash_t`) -> read-only `Label`
 *
 * A field needing a genuinely bespoke widget (its own populate/commit
 * contract, e.g. a curve editor) is out of scope here -- tag it `Hidden`
 * and have the owning app append its own widget as an extra row after this
 * table, exactly as it would have built the whole row by hand before.
 */
class object_inspector : public ui_element {
 public:
  explicit object_inspector(bison::dynamic&& base);

  /**
   * @brief Erase this instance's own built children (the `Table`/rows/
   *        description `Label` `set_target()` last built) from @p s.
   *
   * Without this, every child this instance ever built would outlive it as
   * an orphaned, unreachable `ctx.objects`/`ui_objects` entry (never
   * rendered, never freed) once the caller drops its own reference. Call
   * this explicitly before releasing an instance you are done with --
   * deliberately **not** done automatically from a destructor: this
   * instance may be destroyed as a direct side effect of
   * `bison::rmi::context::objects.clear()` during whole-session teardown
   * (see bison's `server::teardown_session()`), and erasing further entries
   * from `s.objects` *while it is being cleared* is undefined behavior --
   * the same reason no other wish class (`form`, `ui_template`) touches
   * `ctx.objects` from its own destructor either. Skip the call only when
   * you know the whole session is being torn down anyway (every entry,
   * including this instance's own children, gets destroyed together
   * regardless of whether you erase them individually first).
   *
   * Mirrors the explicit (not destructor-driven) teardown convention this
   * codebase already uses for dynamically-stamped widgets elsewhere (e.g.
   * genie's `inspector_window::dynamic_widget_ids_`/`dynamic_widget_paths_`).
   * Safe to call more than once, and safe to call even if nothing was ever
   * built.
   */
  void release(context& s);

  /**
   * @brief Inject session context.
   *
   * Called once by `wish::server::on_create_object` for an RMI-instantiated
   * instance (mirrors `form`/`ui_template`), or directly by server-side C++
   * code that constructs an instance without going through RMI dispatch
   * (must be called before `set_target()` in that case).
   *
   * @param ctx      Per-session RMI context; must outlive `*this`.
   * @param sync_ctx Shared wish session state.
   */
  void init(bison::rmi::context& ctx, sync_context_ptr sync_ctx) {
    ctx_ = &ctx;
    sync_ctx_ = std::move(sync_ctx);
  }

  /// @brief `"__construct"_key` hook body: reads `params["target"]` (if
  ///        present) and builds the initial table. See `handle_instantiate`
  ///        (bison's RMI server) for when this fires.
  bison::dynamic do_construct(const bison::dynamic& params);

  /// @brief `"set_target"_key` method body: `params["target"]`, forwarded
  ///        to `set_target()`.
  bison::dynamic do_set_target(const bison::dynamic& params);

  /**
   * @brief (Re)builds this element's own children -- a `Table` of field
   *        rows plus a description `Label` -- by reflecting over @p
   *        target's registered class. Tearing down any previously-built
   *        children first, so safe to call repeatedly to re-target an
   *        existing instance.
   *
   * @param s       The current session. Caller must already hold the
   *                write lock -- true both for RMI dispatch (this method's
   *                own `"__construct"`/`"set_target"` bodies resolve it via
   *                `require_dispatch_session()`) and for direct C++ callers
   *                holding `context_wlock{*sync_ctx_}` explicitly.
   * @param target  The object to reflect over and edit; may be null (the
   *                table is cleared and the description panel shows a
   *                placeholder).
   */
  void set_target(context& s, bison::dynamic_ptr target);

  /// @brief One field's edited value, resolved and type-coerced by
  ///        `handle_changed()`, ready to hand to the owning app's own
  ///        command/apply logic (e.g. `set_field_command`).
  struct field_edit {
    bison::key_t field_name;
    bison::dynamic new_value; ///< `{field_name: <coerced value>}`, matching
                               ///< the shape most wish apps' field-set
                               ///< commands already expect.
  };

  /// @brief One field's drop-target payload, surfaced verbatim for the
  ///        owning app to resolve (e.g. parse an asset path and load it).
  struct field_drop {
    bison::key_t field_name;
    std::string payload;
  };

  /// @brief Forward this instance's own `Table`'s `"row_selected"`/
  ///        `"row_activated"` event here (with its `{index, ...}` payload);
  ///        no-op if @p widget_id isn't this instance's Table. Updates the
  ///        description panel in place.
  ///
  /// Safe to call from any context (no lock needed): it only mutates this
  /// instance's own already-live `description_label_` field in place, the
  /// same as any leaf widget's render function writing back its own
  /// "value" field -- it does not touch the session's object tree.
  void handle_row_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload);

  /// @brief Returns the coerced edit if @p widget_id is one of this
  ///        instance's field-value widgets and @p payload holds a
  ///        `"value"`/`"text"` (Combo) field, else `std::nullopt`.
  std::optional<field_edit> handle_changed(bison::key_t widget_id, const bison::dynamic& payload) const;

  /// @brief Returns the drop if @p widget_id is one of this instance's
  ///        drop-target widgets, else `std::nullopt`.
  std::optional<field_drop> handle_dropped(bison::key_t widget_id, const bison::dynamic& payload) const;

 private:
  /// @throws std::logic_error if called outside RMI dispatch. Mirrors
  ///         `form::sess()`/`ui_template::sess()` -- only used by this
  ///         class's own `"__construct"`/`"set_target"` method bodies.
  context& require_dispatch_session();

  wish::ui_element_ptr stamp(context& s, bison::key_t klass, const std::string& path);

  bison::rmi::context* ctx_ = nullptr;
  sync_context_ptr sync_ctx_;

  bison::dynamic_ptr target_;
  std::string base_path_;

  std::vector<bison::key_t> built_ids_;
  std::vector<std::string> built_paths_;

  bison::key_t table_id_;
  wish::ui_element_ptr description_label_;
  std::vector<bison::key_t> row_field_order_; ///< Table row index -> field key.
  std::unordered_map<bison::key_t, bison::key_t, bison::key_t, bison::key_t> value_widgets_;
  std::unordered_map<bison::key_t, bison::key_t, bison::key_t, bison::key_t> drop_widgets_;
};

/**
 * @brief Register `object_inspector` as `"ObjectInspector"` in the
 *        `"wish"` bison namespace, parent `"Element"_key`.
 *
 * Renders identically to `VerticalLayout` (its children are always exactly
 * `[Table, Label]`) -- see `built_in_render_fns()` in `imgui_renderer.cpp`.
 */
void register_object_inspector();

} // namespace bdg::wish
