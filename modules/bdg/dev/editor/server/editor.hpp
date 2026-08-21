// MIT License © 2025 Binary Dice Games
/// @file editor.hpp
/// @brief Server-side Editor form.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Live JSON UI mock editor.
///
/// Shows a syntax-highlighted `TextEditor` bound to a sandboxed JSON source
/// file next to a live preview of the UI it describes. The source is
/// re-parsed on every edit (the `TextEditor`'s own `"changed"` event) and
/// every time the client pushes new content via `set_source` (used both for
/// the initial load and to pick up changes made outside the tool -- see the
/// client runner). A successful parse replaces the preview subtree; a failed
/// parse only updates an error banner and leaves the last-good preview
/// untouched, so a syntax typo never makes the preview flicker or vanish.
///
/// Every event fired by a preview widget is appended to an on-screen event
/// log as `"<dot-path> <event>"`, e.g. `"main.ok clicked"` -- this reuses
/// `ui_root::on_event`'s single catch-all dispatch, so it works for any
/// widget type the loaded JSON happens to describe, with no per-type wiring.
///
/// Only Ctrl+S inside the source editor persists to the original local
/// file (mirroring nano's save contract) -- in-editor edits update the
/// live preview immediately but are not written to disk until saved.
/// Closing the window with unsaved edits shows an inline
/// save/discard/cancel confirmation instead of closing immediately.
///
/// The source editor also has `wish_ui_schema` set, which enables autocomplete
/// for element type names, field names, and enum values (backed by
/// `src/ui/ui_schema_help.hpp`'s registry queries) and a `"cursor_moved"`
/// event on every caret move. A separate, independently dockable "Help"
/// `Window` (its own `top_level_objects` entry, not nested inside the main
/// chrome `Window`) shows the enclosing element type's description and a
/// field table (name / category / description columns), updated on every
/// `"cursor_moved"` via `update_help_panel()` -- rebuilt only when the
/// enclosing type actually changes, not on every caret move within the same
/// element. The same event also drives `update_highlight()`, which boxes
/// the preview widget corresponding to the cursor's current element (drawn
/// by `imgui_renderer::render_node()` reading a `"__wish_highlight__"`
/// field it sets on that widget).
///
/// Emitted events:
///   - `"closed"` — user confirmed closing (no unsaved edits, or chose
///     "Discard & Close", or "Save & Close" completed); internal UI
///     (chrome and preview) is removed.
///   - `"on_source_saved"` — Ctrl+S, or "Save & Close" confirmed, inside
///     the source editor; the client should download the sandbox file,
///     persist it to the original local path, and call `mark_saved`
///     (mirrors nano's `on_file_saved`).
class editor : public form {
 public:
  explicit editor(bison::dynamic&& base);

  /// @brief RMI method: point the source `TextEditor` at an already-uploaded
  /// sandbox file and immediately (re)parse it. @p args holds `path`
  /// (string, required, sandbox-relative) and optionally `display_path`
  /// (string; the original local path shown in the filename label). Called
  /// by the client once at startup and again every time the local file
  /// changes outside the tool. Clears the "unsaved changes" state, since
  /// the sandbox content is now known to match disk.
  bison::dynamic do_set_source(const bison::dynamic& args);

  /// @brief RMI method: the client has finished writing the sandbox file
  /// back to the local disk file in response to `on_source_saved`. Clears
  /// the "unsaved changes" state and, if a close was waiting on this save
  /// ("Save & Close"), completes the close.
  bison::dynamic do_mark_saved(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// @brief Read the current source file from the sandbox and try to import
  /// it. On success, replaces the preview subtree and clears the error
  /// banner. On failure (invalid JSON / unknown element type), sets the
  /// banner to the error message and leaves any existing preview alone.
  void try_reparse();

  /// @brief Remove the preview subtree (ui_objects, top_level_objects,
  /// top_level_handlers) and clear `mock_id_to_path_`. Safe to call when
  /// there is no preview yet.
  void clear_mock();

  /// @brief Append one row to the event log table. If the table already
  /// holds `kMaxLogRows` rows, the oldest one is discarded first, so the
  /// log stays bounded during a long-running session.
  void append_log_row(const std::string& text);

  /// @brief Set (or clear, with an empty string) the error banner text.
  void set_banner(const std::string& text);

  /// @brief Refresh the filename label's text from `display_path_`/`dirty_`.
  void update_path_label();

  /// @brief Recompute the Help window's contents for whatever JSON element
  /// encloses (line, column) in `current_source_content_`, using
  /// `ui_schema_help`'s cursor-context scanner and class registry query.
  /// A no-op if the enclosing type is unchanged from the last call (see
  /// `last_help_type_`); otherwise clears the field table (`clear_help_rows()`)
  /// and rebuilds it via `append_help_row()`, one row per field. Clears the
  /// title/description and empties the table if no enclosing element type is
  /// found (or the type isn't a registered class -- e.g. still being typed).
  void update_help_panel(int32_t line, int32_t column);

  /// @brief Remove every row currently in the Help window's field table
  /// (`help_table_ptr_`'s `children`, plus each row/cell's `ctx().objects`
  /// entry) and reset `next_help_child_key_` to 0. Safe to call when the
  /// table is already empty.
  void clear_help_rows();

  /// @brief Append one row to the Help window's field table: a field-name
  /// cell (colored via `Label.text_color`, `" *"`-suffixed when @p required),
  /// a category cell, and a wrapped description cell (range/enum annotations
  /// folded in, same bracket format `format_class_help()` used previously).
  void append_help_row(
      const std::string& field_name, bool required, const std::string& category, const std::string& description);

  /// @brief Move the preview highlight box to the element at @p new_path
  /// (`std::nullopt` clears it without setting a new one). Clears the
  /// `"__wish_highlight__"` field on the previously-highlighted element (if
  /// any still exists in the current preview tree) and sets it on the new
  /// one, via a direct `s.ui_objects` dot-path lookup -- safe to call at any
  /// time, not just right after a reparse. No-op if @p new_path already
  /// matches `highlighted_path_`.
  void update_highlight(const std::optional<std::string>& new_path);

  /// @brief Tear down chrome and preview and emit `"closed"`. The actual
  /// close action, run once no confirmation is needed (or the user has
  /// resolved one via discard/save).
  void request_close();

  /// @brief Run @p fn with a valid `wish::context&`, whether called from
  /// RMI dispatch (`do_set_source`, where `sess()` already works) or from
  /// `on_event()` -- which the render loop calls *outside* the session
  /// lock (see `form::on_event`'s doc comment), so `sess()` would throw
  /// there. Mirrors `form::remove_internal_objects()`'s own dispatch /
  /// non-dispatch branch.
  void with_session(const std::function<void(context&)>& fn);

  bison::key_t window_id_;
  bison::key_t source_editor_id_;
  ui_element_ptr source_editor_ptr_;
  ui_element_ptr banner_ptr_;
  ui_element_ptr path_label_ptr_;

  // Help and Event Log windows: separate top-level Windows (own
  // top_level_objects/top_level_handlers entries, not nested under
  // window_id_'s chrome tree), built once in on_init() and torn down
  // explicitly in request_close() (form::remove_internal_objects() only
  // cleans up internal_root_key_) -- see help_root_key_/log_root_key_'s
  // doc comments.
  ui_element_ptr help_class_name_ptr_;
  ui_element_ptr help_class_desc_ptr_;
  ui_element_ptr help_table_ptr_;
  ui_element_ptr log_table_ptr_;

  // Inline close-confirmation panel (shown in place of a true modal dialog
  // -- see editor.cpp's layout comment) and its three buttons.
  ui_element_ptr confirm_panel_ptr_;
  bison::key_t confirm_save_id_;
  bison::key_t confirm_discard_id_;
  bison::key_t confirm_cancel_id_;

  std::string current_source_path_; // sandbox-relative path of the source file
  std::string current_source_content_; // last content read in try_reparse(), reused by
                                        // update_help_panel() to avoid a disk read on every
                                        // cursor move (which fires far more often than edits)
  std::string display_path_; // original local path, shown in the filename label
  bool dirty_{false}; // true once the source has unsaved in-editor edits
  bool pending_close_after_save_{false}; // "Save & Close" is waiting on mark_saved()
  std::string mock_root_key_; // top_level_objects key for the preview subtree

  /// `top_level_objects`/`top_level_handlers` key for the Help window's own
  /// root -- set once in `on_init()` (`internal_root_key_ + "_help"`,
  /// mirroring `mock_root_key_`'s naming), never rebuilt afterwards (unlike
  /// the preview, the Help window's own chrome doesn't change across
  /// reparses -- only its content, via `update_help_panel()`).
  std::string help_root_key_;

  /// `top_level_objects`/`top_level_handlers` key for the event log
  /// window's own root -- set once in `on_init()`
  /// (`internal_root_key_ + "_log"`), same pattern as `help_root_key_`.
  std::string log_root_key_;

  /// The enclosing type `update_help_panel()` last rebuilt the field table
  /// for (empty if the table is currently empty/cleared). Lets a
  /// `"cursor_moved"` that stays within the same element skip rebuilding
  /// the table (fresh RMI ids/`ctx().objects` entries for every row) when
  /// nothing would actually change.
  std::string last_help_type_;

  /// `__wish_id` reused for the preview root `Window` across successive
  /// reparses. ImGui keys a window's position/size/focus state off its
  /// label + this id (see `with_id()` in imgui_ui_renderer.cpp); allocating
  /// a fresh id every reparse made ImGui treat each reparse as a brand-new
  /// window, resetting any position/size the user had dragged it to and
  /// stealing focus away from the source editor. Reusing the id keeps
  /// ImGui's own per-window state keyed continuously across reparses.
  bison::key_t mock_window_id_;

  static constexpr size_t kMaxLogRows = 200;

  // Bookkeeping for one live log row, enough to fully evict it: its slot in
  // the log table's "children" map plus every RMI id put_object() assigned
  // it (the row itself and its two cells), so an eviction doesn't just hide
  // the row from the table but also erases it from ctx().objects -- leaving
  // those behind would silently leak three ctx().objects entries per
  // discarded row over a long session, defeating the point of capping.
  struct log_row_entry {
    size_t child_key;
    bison::key_t row_id;
    bison::key_t cell_seq_id;
    bison::key_t cell_text_id;
  };

  size_t log_seq_{0};
  size_t next_log_child_key_{0};
  std::deque<log_row_entry> log_rows_; // oldest first

  // Bookkeeping for one live Help-window field-table row, mirroring
  // log_row_entry above -- enough to fully erase it (its slot in the field
  // table's "children" map, plus every RMI id put_object() assigned it: the
  // row itself and its three cells). Unlike the log (FIFO-capped, entries
  // evicted one at a time), the whole table is cleared and rebuilt at once
  // (clear_help_rows()) whenever the enclosing type changes, so there is no
  // analogous "oldest first" ordering requirement.
  struct help_row_entry {
    size_t child_key;
    bison::key_t row_id;
    bison::key_t field_cell_id;
    bison::key_t category_cell_id;
    bison::key_t desc_cell_id;
  };

  size_t next_help_child_key_{0};
  std::vector<help_row_entry> help_rows_;

  /// `__wish_id.id` -> dot-path within the preview tree, rebuilt on every
  /// successful reparse; used to resolve a preview widget's event back to a
  /// human-readable path for the log.
  std::unordered_map<uint32_t, std::string> mock_id_to_path_;

  /// Dot-path of the preview element currently showing the cursor
  /// highlight box, or `std::nullopt` if none. Survives across reparses
  /// (each of which tears down and rebuilds the whole preview tree) by
  /// being reapplied inline in `try_reparse()`'s own per-element loop --
  /// see `update_highlight()`.
  std::optional<std::string> highlighted_path_;
};

/// @brief Register Editor in the "wish" bison namespace.
void register_editor();

} // namespace bdg::wish
