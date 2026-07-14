// MIT License © 2025 Binary Dice Games
/// @file editor.hpp
/// @brief Server-side Editor form.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

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
/// Emitted events:
///   - `"closed"` — user clicked the window X button; internal UI (chrome
///     and preview) is removed.
///   - `"on_source_saved"` — Ctrl+S inside the source editor; the client
///     should download and persist the sandbox file to the original local
///     path (mirrors Notepad's `on_file_saved`).
class editor : public form {
 public:
  explicit editor(bison::dynamic&& base);

  /// @brief RMI method: point the source `TextEditor` at an already-uploaded
  /// sandbox file and immediately (re)parse it. @p args holds `path`
  /// (string, required, sandbox-relative). Called by the client once at
  /// startup and again every time the local file changes outside the tool.
  bison::dynamic do_set_source(const bison::dynamic& args);

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

  /// @brief Append one row to the event log table.
  void append_log_row(const std::string& text);

  /// @brief Set (or clear, with an empty string) the error banner text.
  void set_banner(const std::string& text);

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
  ui_element_ptr log_table_ptr_;

  std::string current_source_path_; // sandbox-relative path of the source file
  std::string mock_root_key_; // top_level_objects key for the preview subtree

  size_t log_seq_{0};
  size_t next_log_child_key_{0};

  /// `__wish_id.id` -> dot-path within the preview tree, rebuilt on every
  /// successful reparse; used to resolve a preview widget's event back to a
  /// human-readable path for the log.
  std::unordered_map<uint32_t, std::string> mock_id_to_path_;
};

/// @brief Register Editor in the "wish" bison namespace.
void register_editor();

} // namespace bdg::wish
