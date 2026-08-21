// MIT License © 2025 Binary Dice Games
/// @file git.hpp
/// @brief Server-side GitRepo form -- a SourceTree-style git GUI.
///
/// All git invocation and output parsing happens client-side (see
/// client/git_repo_source.hpp) -- the machine whose repo state matters is the
/// user's own. This form only renders whatever snapshot it was last given via
/// its update_refs/update_log/update_status/update_commit_files/update_diff
/// RMI methods, and emits high-level *_requested events the client reacts to
/// by running the corresponding git command and pushing a fresh snapshot.
///
/// wish's only built-in dialog form, MessageBox (src/ui/forms/
/// message_box.hpp), is a genuine modal (Window.modal = true) but has no
/// slot for custom body content -- just a title, message, icon, and a
/// Win32-style button preset -- so it can't host a branch-name field or a
/// branch picker. Branch creation and merge target selection therefore use
/// small always-visible inline controls (a name InputText next to the
/// sidebar's Branches header, and a "last selected branch" tracked from
/// sidebar clicks) instead of a dialog -- documented as a deliberate V1
/// simplification in this module's DESIGN.md, not an oversight. Destructive
/// actions (delete branch, stash drop) do use a real modal: an inline
/// confirm Window built into this form's own tree, mirroring
/// tree::show_overwrite_confirm()'s pattern -- see show_confirm()
/// below.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/// @brief SourceTree-style git GUI form.
///
/// Owns four independently dockable Windows (mirrors the `editor` module's
/// chrome/Help/Event-log split): the main window (toolbar, branches/tags/
/// stashes/remotes sidebar, commit graph), a Files window (staged/unstaged
/// working-directory files, or a selected commit's changed files), a Diff
/// window (the selected file's diff), and a Log window (every `git`
/// subprocess invocation the client makes, for debugging/tracing -- see
/// append_command_log()). All four share this same git_repo instance as
/// their event handler.
///
/// Emitted events:
///   - `"closed"` — user closed the main window; all three internal
///     subtrees are removed.
///   - `"stage_requested"` / `"unstage_requested"` — `{ path: string }`.
///   - `"commit_requested"` — `{ message: string }`.
///   - `"checkout_requested"` — `{ ref: string }`.
///   - `"create_branch_requested"` — `{ name: string, start_point: string }` (start_point empty == HEAD).
///   - `"delete_branch_requested"` — `{ name: string, force: bool }`.
///   - `"fetch_requested"` / `"pull_requested"` / `"push_requested"` — no payload.
///   - `"merge_requested"` — `{ ref: string }`.
///   - `"stash_push_requested"` — no payload (client uses a default message).
///   - `"stash_pop_requested"` / `"stash_apply_requested"` / `"stash_drop_requested"` — `{ index: int32 }`.
///   - `"refresh_requested"` — no payload; fired once on open and on the
///     toolbar Refresh button.
///   - `"commit_files_requested"` — `{ hash: string }`; the client should
///     call update_commit_files() with that commit's changed files.
///   - `"diff_requested"` — `{ hash: string (empty = working tree), path: string, staged: bool }`;
///     the client should call update_diff() with that file's diff.
class git_repo : public form {
 public:
  explicit git_repo(bison::dynamic&& base);

  /// @brief RMI method: replace the sidebar's ref data. @p args holds:
  ///   - `current_branch` (string).
  ///   - `branches` (dynamic array), each `{ name, is_remote (bool),
  ///     upstream (string, may be empty), ahead (int32), behind (int32) }`
  ///     -- `is_remote` branches populate the Remotes sidebar section
  ///     (remote-tracking branch names, e.g. "origin/main"); there is no
  ///     separate bare-remote-name (no branch) entry in this pass.
  ///   - `tags` (dynamic array), each `{ name }`.
  ///   - `stashes` (dynamic array), each `{ index (int32), message }`.
  bison::dynamic do_update_refs(const bison::dynamic& args);

  /// @brief RMI method: replace the commit graph. @p args holds:
  ///   - `commits` (dynamic array), each `{ hash, parents (array of hash
  ///     strings), author, date (preformatted string), subject }`, in
  ///     display order (newest first, topologically sorted).
  ///   - `working_dirty` (bool) — true if the working tree has uncommitted
  ///     changes; prepends a synthetic "Uncommitted changes" row.
  bison::dynamic do_update_log(const bison::dynamic& args);

  /// @brief RMI method: replace the Files panel with the working directory's
  /// staged/unstaged files (stage/unstage checkboxes shown). @p args holds:
  ///   - `staged` (dynamic array), each `{ path, status (single-char code) }`.
  ///   - `unstaged` (dynamic array), same shape (includes untracked files).
  /// Cached verbatim (last_status_args_) so re-selecting the working-tree
  /// graph row can redisplay it without a fresh client round trip.
  bison::dynamic do_update_status(const bison::dynamic& args);

  /// @brief RMI method: replace the Files panel with one commit's changed
  /// files (read-only, no checkboxes). @p args holds:
  ///   - `hash` (string).
  ///   - `files` (dynamic array), each `{ path, status (single-char code) }`.
  bison::dynamic do_update_commit_files(const bison::dynamic& args);

  /// @brief RMI method: replace the Diff panel's content. @p args holds:
  ///   - `path` (string) — shown as the panel title.
  ///   - `lines` (dynamic array), each `{ kind ("add"/"del"/"context"/"header"), text }`.
  bison::dynamic do_update_diff(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run git command (e.g.
  /// push/pull/fetch/merge), shown in the status label. @p args holds:
  ///   - `command` (string) — e.g. "push".
  ///   - `ok` (bool).
  ///   - `output` (string) — combined stdout/stderr, shown on failure.
  bison::dynamic do_command_result(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Log window, reporting a
  /// single `git` subprocess invocation the client just made. Unlike
  /// command_result (which only reports the one mutating command behind a
  /// user action, for the status label), this is called by the client for
  /// *every* git invocation -- including the read-only ones (status, log,
  /// for-each-ref, ...) issued by refresh_all() -- so the Log window is a
  /// complete trace, for debugging/tracing the tool itself. @p args holds:
  ///   - `command` (string) — the full argv, e.g. "git status --porcelain=v1".
  ///   - `exit_code` (int32).
  ///   - `ok` (bool).
  ///   - `output` (string) — trimmed, single-line, length-capped preview of
  ///     stdout (or stderr on failure).
  bison::dynamic do_append_command_log(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Window construction ─────────────────────────────────────────────────
  void build_main_window();
  void build_files_window();
  void build_diff_window();
  void build_log_window();

  // ── Confirmation modal ──────────────────────────────────────────────────
  // Mirrors tree::show_overwrite_confirm()/request_close_confirm()/
  // remove_confirm_objects() (tree.cpp) exactly: a second modal
  // Window built directly into this form's own tree, not a standalone
  // MessageBox instantiation (see this file's top-of-file comment and
  // DESIGN.md §6 for why). A single pending_confirm_action_ replaces
  // mc's enum-typed pending_transfer branching, since only one
  // reusable "are you sure?" shape is needed here.

  /// @brief Builds and shows the confirm modal with @p message and a
  /// confirm button captioned @p confirm_label; @p on_confirm runs if the
  /// user confirms. Called from on_event() (outside dispatch, per
  /// form.hpp's documented contract), so session state is reached via
  /// context_wlock{*sync_ctx_}, not sess() -- exactly matching
  /// show_overwrite_confirm()'s own dispatch/non-dispatch handling.
  void show_confirm(const std::string& message, const std::string& confirm_label, std::function<void()> on_confirm);
  /// @brief Thin wrapper over form::request_close_at(confirm_root_key_).
  void request_close_confirm();
  /// @brief Thin wrapper over form::remove_objects_at(confirm_root_key_).
  void remove_confirm_objects();

  // ── Sidebar ──────────────────────────────────────────────────────────────
  struct sidebar_row {
    ui_element_ptr row; // HorizontalLayout: Selectable + MenuButton
    size_t child_key{0};
  };
  /// @brief Clears @p rows' entries out of @p section's children map and
  /// rebuilds one row per index in `[0, count)` by calling `make_row(index)`
  /// for a fully-built row (see make_sidebar_row()).
  void rebuild_section(
      const ui_element_ptr& section,
      std::vector<sidebar_row>& rows,
      size_t count,
      const std::function<ui_element_ptr(size_t index)>& make_row);
  /// @brief Builds one sidebar row: a HorizontalLayout of a Selectable
  /// (label, `on_click`) and a MenuButton listing `menu_items` (label + callback
  /// pairs). Assigns ids and wires click_handlers_/selectable_handlers_.
  ui_element_ptr make_sidebar_row(
      const std::string& label,
      std::function<void()> on_click,
      const std::vector<std::pair<std::string, std::function<void()>>>& menu_items);
  ui_element_ptr make_menu_item(const std::string& label, std::function<void()> on_click);
  void assign_id(const ui_element_ptr& el);

  // ── Graph ────────────────────────────────────────────────────────────────
  void rebuild_graph_table(const bison::dynamic& args);
  void select_row(int32_t index);

  // ── Status / commit files ───────────────────────────────────────────────
  struct file_row_entry {
    ui_element_ptr row;
    std::string path;
    bool staged{false};
  };
  void clear_file_rows();
  /// @brief Adds one row: column 0 is a Checkbox (working mode, wired to
  /// checkbox_handlers_ -> stage_requested/unstage_requested) or a plain
  /// status-code Label (read-only commit-files mode); column 1 is always a
  /// Selectable (not a plain Label) so the row's path can be individually
  /// clicked for diff selection without relying on Table's own row-level
  /// hit test, which would otherwise compete with the checkbox's own click
  /// handling in the same row.
  void add_file_row(const std::string& path, const std::string& status, bool staged, bool show_checkbox);
  void request_diff_for_selected();

  // ── Diff ─────────────────────────────────────────────────────────────────
  void clear_diff_rows();

  // ── Log (git-command trace) ─────────────────────────────────────────────
  // Mirrors the `editor` module's own event-log Table (editor.cpp's
  // append_log_row()/kMaxLogRows/log_row_entry): a FIFO-capped Table so a
  // long-running session's Log window stays bounded instead of growing
  // without limit.

  /// @brief Appends one row (sequence #, command, exit code, output
  /// preview) to the Log window's table, color-coded green/red by @p ok.
  /// Evicts the oldest row first once the table already holds kMaxLogRows.
  void append_log_row(const std::string& command, int32_t exit_code, bool ok, const std::string& output);

  std::string files_root_key_;
  std::string diff_root_key_;
  std::string log_root_key_;

  // Confirmation modal (empty confirm_root_key_ == not currently shown)
  std::string confirm_root_key_;
  bison::key_t confirm_window_id_;
  bison::key_t confirm_yes_id_;
  bison::key_t confirm_no_id_;
  std::function<void()> pending_confirm_action_;

  // Main window
  bison::key_t window_id_;
  ui_element_ptr status_label_;
  ui_element_ptr current_branch_label_;
  ui_element_ptr sidebar_branches_section_;
  ui_element_ptr sidebar_remotes_section_;
  ui_element_ptr sidebar_tags_section_;
  ui_element_ptr sidebar_stashes_section_;
  std::vector<sidebar_row> branch_rows_;
  std::vector<sidebar_row> remote_rows_;
  std::vector<sidebar_row> tag_rows_;
  std::vector<sidebar_row> stash_rows_;
  ui_element_ptr new_branch_input_;
  bison::key_t new_branch_input_id_;
  std::string new_branch_name_text_;
  std::string selected_branch_; // last sidebar branch row clicked; merge target.
  ui_element_ptr graph_table_;
  bison::key_t graph_table_id_;
  std::vector<std::string> graph_row_hashes_; // "" marks the synthetic working-tree row

  // Files window
  bison::key_t files_window_id_;
  ui_element_ptr files_title_label_;
  ui_element_ptr files_table_;
  ui_element_ptr commit_message_input_;
  bison::key_t commit_message_input_id_;
  std::string commit_message_text_;
  ui_element_ptr commit_button_;
  bison::key_t commit_button_id_;
  std::vector<file_row_entry> file_rows_;
  bool status_mode_working_{true};
  bison::dynamic last_status_args_; // cache: redisplay on re-selecting the working-tree row.

  // Diff window
  bison::key_t diff_window_id_;
  ui_element_ptr diff_title_label_;
  ui_element_ptr diff_table_;

  std::string selected_hash_; // "" == working tree, or no selection yet
  std::string selected_path_;
  bool selected_staged_{false};

  // Log window
  bison::key_t log_window_id_;
  ui_element_ptr log_table_;

  static constexpr size_t kMaxLogRows = 500;

  // Bookkeeping for one live Log-window row, enough to fully evict it: its
  // slot in the log table's "children" map plus every RMI id put_object()
  // assigned it (the row itself and its four cells) -- mirrors editor.hpp's
  // own log_row_entry, see that type's doc comment for why this is needed
  // (an eviction that only hid the row from the table, without also erasing
  // its ctx().objects entries, would leak them over a long session).
  struct log_row_entry {
    size_t child_key;
    bison::key_t row_id;
    bison::key_t cell_seq_id;
    bison::key_t cell_command_id;
    bison::key_t cell_exit_id;
    bison::key_t cell_output_id;
  };

  size_t log_seq_{0};
  size_t next_log_child_key_{0};
  std::deque<log_row_entry> log_rows_; // oldest first

  // Dispatch tables, populated in on_init()/rebuild_* -- keyed by widget id.
  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
  std::unordered_map<bison::key_t, std::function<void(bool)>, bison::key_t, bison::key_t> checkbox_handlers_;
  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> selectable_handlers_;

  // Each dynamic children map needing generated numeric keys gets its own
  // contiguous 0-based counter -- dynamic::size() (bison_object.hpp)
  // reports "highest numeric key + 1", not a true element count, so a
  // counter shared across independent containers would make every later
  // one's size() over-report. The sidebar sections and the graph/diff
  // tables are fully rebuilt in one shot each time (rebuild_section(),
  // rebuild_graph_table(), do_update_diff()), so a local counter declared
  // at the top of each of those suffices; only the files table is
  // populated by repeated add_file_row() calls across a loop in the
  // caller, so it alone needs a persistent per-table counter here.
  size_t next_file_row_key_{0};
};

/// @brief Register GitRepo in the "wish" bison namespace.
void register_git();

} // namespace bdg::wish
