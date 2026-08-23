// MIT License © 2025 Binary Dice Games
/// @file nano.hpp
/// @brief Server-side form for nano (a multi-file text editor).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief Multi-file, syntax-highlighted text editor form.
///
/// Files edited by this form live in the session's sandboxed resource
/// directory (`context::resource_dir`); the connected client is responsible
/// for moving bytes into and out of that sandbox via `client::upload_file` /
/// `client::download_file`. Opening a file is therefore a two-step
/// handshake: the client uploads the file's contents, then calls the
/// `open_file` method with the resulting sandbox-relative path. Each open
/// file is rendered as one closable tab (`TabItem`) containing a language
/// `Combo` above a `TextEditor`, which already reads/writes the sandbox
/// file directly and provides syntax highlighting — this form only manages
/// tabs (and which language each editor highlights as), it does not
/// duplicate `TextEditor`'s load/save logic. The combo is seeded from the
/// file's extension (see `language_for_extension()` in nano.cpp) but the
/// user may override it at any time; doing so does not mark the file dirty,
/// since it changes only how the existing content is displayed.
///
/// The Nano never lists a client's local directory itself (it has no way
/// to). Clicking its "Open" button emits `on_request_open` so the client can
/// present its own file picker (e.g. driven by the `FileDialog` form,
/// populated from the client's local filesystem) and upload the chosen file
/// before calling `open_file`. Clicking "New" emits `on_request_new`, the
/// same handshake but asking the client to pick (and create) a fresh local
/// file instead of an existing one. Clicking "Save" emits `on_file_saved`
/// for the currently active tab's file only, asking the client to download
/// and persist just that one file. Closing a tab, or the whole window,
/// emits `on_file_closed` per file so the client can download it one last
/// time before discarding its local bookkeeping.
///
/// Each open file tracks whether it has unsaved changes: a tab's label gets
/// a `" *"` suffix while its `TextEditor` has been edited (the `"changed"`
/// event) since the last save (Ctrl+S, the "Save" button, or the file being
/// flushed on close), and clears again once saved. If the whole window is
/// closed (its title-bar X) while any tab still has unsaved changes, the
/// close is not completed immediately -- `on_confirm_close` is emitted
/// instead (payload: `paths`, the list of unsaved files) so the client can
/// ask the user whether to save them, discard them, or cancel the close
/// entirely. `confirm_close(save)` finishes the close: `save: true` flushes
/// every open file (identical to today's unconditional close-flush);
/// `save: false` flushes only the already-saved files, leaving each unsaved
/// file's local copy untouched. A canceled close simply never calls
/// `confirm_close` -- the window was never removed from `top_level_objects`
/// while waiting on the prompt, so leaving it unanswered leaves the window
/// open exactly as it was, with every tab and its unsaved changes intact.
class nano : public form {
 public:
  explicit nano(bison::dynamic&& base);

  /// @brief RMI method: register an already-uploaded sandbox file as a new
  /// tab. @p args holds `path` (string, required, sandbox-relative) and an
  /// optional `title` (string; defaults to `path`). Emits `on_file_opened`
  /// on success or `on_error` if the path is empty or escapes the sandbox.
  /// A no-op if `path` is already open.
  bison::dynamic do_open_file(const bison::dynamic& args);

  /// @brief RMI method: the user's answer to the `on_confirm_close` prompt.
  /// @p args holds `save` (bool). Completes the window close that was
  /// deferred when it was emitted -- see the class doc comment above.
  bison::dynamic do_confirm_close(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct open_file_entry {
    std::string path; // sandbox-relative path
    std::string title; // tab label, without the unsaved-changes suffix
    bison::key_t tab_id; // TabItem __wish_id
    bison::key_t editor_id; // TextEditor __wish_id
    ui_element_ptr tab_ptr; // TabItem element, for removal from tab_bar
    size_t child_key; // key this TabItem was stored under in tab_bar's children
    bool dirty{false}; // true once edited since the last save
    ui_element_ptr editor_ptr; // TextEditor element, to retarget its "language" field
    bison::key_t lang_combo_id; // language-select Combo __wish_id
  };

  /// @brief Remove the tab at @p index from the UI tree and tracking state,
  /// then emit `on_file_closed` for it.
  void close_file_at(size_t index);

  /// @brief Rebuild tab_id_to_index_ / editor_id_to_index_ / lang_combo_id_to_index_
  /// from open_files_.
  void rebuild_index_maps();

  /// @brief Refresh the TabItem label at @p index from its title/dirty state.
  void update_tab_label(size_t index);

  /// @brief Flush and tear down every open file, then emit `"closed"` and
  /// remove the internal UI tree. @p flush_dirty_files selects whether a
  /// still-unsaved file is flushed (downloaded by the client) too, or
  /// skipped so its local copy is left untouched -- see the class doc
  /// comment's `on_confirm_close` / `confirm_close` description.
  void do_close(bool flush_dirty_files);

  ui_element_ptr tab_bar_ptr_;
  bison::key_t window_id_;
  bison::key_t btn_open_id_;
  bison::key_t btn_new_id_;
  bison::key_t btn_save_id_;
  bison::key_t active_tab_id_; // TabItem __wish_id of the currently selected tab
  size_t next_child_key_{0};

  std::vector<open_file_entry> open_files_;
  std::unordered_map<uint32_t, size_t> tab_id_to_index_;
  std::unordered_map<uint32_t, size_t> editor_id_to_index_;
  std::unordered_map<uint32_t, size_t> lang_combo_id_to_index_;
};

/// @brief Register Nano in the "wish" bison namespace.
void register_nano();

} // namespace bdg::wish
