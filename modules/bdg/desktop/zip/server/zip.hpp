// MIT License © 2025 Binary Dice Games
/// @file zip.hpp
/// @brief Server-side form for zip (UI only, no local file access).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace bdg::wish {

class message_box;

/// @brief Client-machine file browser with compress/extract/view-contents
/// actions for zip archives.
///
/// Unlike mc's sandbox (right) panel or Zip's own first draft,
/// this form has **no filesystem access at all** -- the files it browses
/// live on the *client's* local machine, which the server cannot reach
/// directly. It follows exactly the handshake mc's local (left)
/// panel uses: the form emits `on_navigate` when the user wants to browse a
/// different directory, and the client responds by calling
/// `update_listing()` with the freshly enumerated contents. Selecting a row
/// only tracks state; the actual compress/extract/list-contents work only
/// happens once the user clicks the corresponding button, which emits
/// `on_compress_requested`/`on_extract_requested`/
/// `on_view_contents_requested` for the client to act on (see
/// `modules/bdg/desktop/zip/client/zip.cpp` for the reference
/// client, which does the actual miniz-based zip I/O).
///
/// The file table supports mc-style multi-row selection (see
/// apply_row_click()): a plain click replaces the selection with just that
/// row, Ctrl+click toggles one row, Shift+click/drag range-selects between
/// the last plain-clicked row and the hovered one. Compress acts on every
/// selected file/folder (the ".." pseudo-row is skipped); Extract and View
/// Contents only act when exactly one `.zip` file is selected.
///
/// The "already exists?" overwrite check (for both Compress's archive name
/// and Extract's destination folder name) is answered from the cached
/// listing last reported via `update_listing()`, not a filesystem probe --
/// this form never touches disk, so it has nothing else to check against.
///
/// While a compress/extract is in flight, the client streams per-file
/// progress back via `set({"progress": ..., "progress_label": ...,
/// "status": ...})` -- `progress` (0..1) and `progress_label` drive the
/// progress bar at the bottom of the window, while `status` names the file
/// currently being processed, mirroring mc's `transfer_progress`/
/// `transfer_label` fields for its own upload/download transfers.
///
/// Emitted events:
///   - `"closed"` — window X button; internal UI removed.
///   - `"on_navigate"` (`{name, type}`, `type` is `"dir"` or `"path"`) —
///     client should re-list the target directory and call
///     `update_listing()`.
///   - `"on_compress_requested"` (`{path, source_names, archive_name}`,
///     `source_names` a plain-string array) — client should create
///     `path/archive_name` containing every `path/<name>` in `source_names`
///     (recursively, for a directory), reporting per-file progress as
///     described above, then report the outcome via `set({"status": ...})`
///     and refresh via `update_listing()`.
///   - `"on_extract_requested"` (`{path, zip_name, dest_name}`) — client
///     should extract `path/zip_name` into `path/dest_name`, reporting
///     per-file progress the same way, then report the outcome the same way.
///   - `"on_view_contents_requested"` (`{path, name}`) — client should read
///     `path/name`'s central directory (without extracting) and call
///     `show_contents(name, entries)`.
class zip : public form {
 public:
  explicit zip(bison::dynamic&& base);

  /// @brief RMI method: replace the browser's displayed directory listing.
  /// @p args holds `path` (string) and `files` (dynamic array of entries,
  /// each `{name, type ("file"/"dir"), size, modified}` — `size`/`modified`
  /// are already client-formatted display strings, mirroring
  /// mc's `update_local_listing()`).
  bison::dynamic do_update_listing(const bison::dynamic& args);

  /// @brief RMI method: open the "View Contents" dialog for the archive
  /// named @p args's `name`, populated from @p args's `entries` (dynamic
  /// array, each `{name, type ("file"/"dir"), uncompressed_size,
  /// compressed_size}`, as read by the client from the archive's central
  /// directory).
  bison::dynamic do_show_contents(const bison::dynamic& args);

  /// @brief Called from the `__setter` prototype method for every set() call.
  /// Intercepts `status` to mirror it into the internal status label, and
  /// `progress`/`progress_label` to mirror them into the internal progress
  /// bar.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct file_row {
    std::string name;
    std::string type; ///< "file" or "dir"
    std::string size;
    std::string modified;
  };

  /// @brief One entry from an archive's central directory, as reported by
  /// the client for the "View Contents" table.
  struct archive_entry {
    std::string name;
    bool is_dir{false};
    std::uint64_t uncompressed_size{0};
    std::uint64_t compressed_size{0};
  };

  /// @brief What the name/destination prompt dialog, once confirmed, should
  /// do -- mirrors tree.cpp's `pending_transfer`.
  enum class pending_action { none, compress, extract };

  void fill_table(const std::vector<file_row>& entries, const std::set<std::string>& selected_names = {});
  void fill_contents_table(const ui_element_ptr& table, const std::vector<archive_entry>& entries) const;
  void set_status(const std::string& message);
  bool is_zip_name(const std::string& name) const;

  /// @brief Look up @p name in the last listing reported via
  /// `update_listing()`, returning `"dir"`/`"file"`, or `""` if not present.
  /// The overwrite-confirmation checks below use this instead of a
  /// filesystem probe, since this form has no filesystem to probe.
  std::string cached_entry_type(const std::string& name) const;

  /// @brief Sorts @p entries in place by the given file_table column
  /// (0=Name, 1=Size, 2=Modified), leaving a leading ".." entry pinned
  /// first -- see tree.cpp's sort_entries() for the reference
  /// this mirrors.
  void sort_entries(std::vector<file_row>& entries, int32_t sort_column_id, bool ascending) const;
  void on_table_sorted(const bison::dynamic& payload);

  // ── Multi-selection ──────────────────────────────────────────────────────
  //
  // Mirrors mc.cpp's own apply_row_click()/describe_selection() exactly --
  // see mc.hpp's class doc comment and apply_row_click()'s doc comment there
  // for the full Ctrl/Shift semantics this reproduces.

  /// @brief Applies one row click's multi-selection semantics to @p
  /// selected_names_/@p selection_anchor_, given the clicked row's @p idx
  /// plus the Ctrl/Shift modifier state from the click's payload.
  static void apply_row_click(
      std::set<std::string>& selected, int32_t& anchor, const std::vector<file_row>& entries, int32_t idx, bool ctrl,
      bool shift);

  /// @brief Formats the "Selected: ..." label text for the current
  /// multi-selection: "(none)", the single name, or "N items".
  static std::string describe_selection(const std::set<std::string>& selected);

  /// @brief Names in @p selected present in @p entries, excluding the ".."
  /// pseudo-row -- what the Compress button acts on (unlike mc's
  /// selected_file_names(), directories are included: Compress archives
  /// files and folders alike).
  static std::vector<std::string> selected_target_names(
      const std::set<std::string>& selected, const std::vector<file_row>& entries);

  /// @brief Builds a `{names: [string...]}` event payload from @p names, the
  /// same convention as mc.cpp's make_names_payload().
  static bison::dynamic make_names_payload(const std::vector<std::string>& names);

  // ── Name/destination prompt dialog (Compress/Extract, first step) ────────
  void show_prompt(
      pending_action action, const std::vector<std::string>& source_names, const std::string& default_value);
  void request_close_prompt();
  void remove_prompt_objects();
  void on_prompt_confirmed();

  // ── Overwrite confirmation dialog (Compress/Extract, second step) ────────
  void show_overwrite_confirm(
      pending_action action, const std::vector<std::string>& source_names, const std::string& target_name,
      const std::string& message);

  /// @brief Emit the compress/extract request event for @p action, using
  /// @p target_name as the archive name (compress) or destination folder
  /// name (extract).
  void emit_action_request(
      pending_action action, const std::vector<std::string>& source_names, const std::string& target_name);

  // ── View Contents dialog ──────────────────────────────────────────────────
  void show_contents_dialog(const std::string& zip_name, const std::vector<archive_entry>& entries);
  void request_close_contents();
  void remove_contents_objects();

  bison::key_t window_id_;
  bison::key_t path_input_id_;
  bison::key_t file_table_id_;
  bison::key_t btn_compress_id_;
  bison::key_t btn_extract_id_;
  bison::key_t btn_view_id_;
  bison::key_t btn_refresh_id_;

  ui_element_ptr path_input_ptr_;
  ui_element_ptr file_table_ptr_;
  ui_element_ptr selected_label_ptr_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr progress_ptr_;

  std::string path_; ///< Last directory path reported by the client via update_listing().
  std::vector<file_row> entries_;
  int32_t sort_column_id_{0};
  bool sort_ascending_{true};

  // Multi-selection state: the set of currently-selected entry names
  // (name-keyed, not index-keyed, so it survives a re-sort -- see
  // on_table_sorted()'s doc comment), plus the row index Shift+click/drag
  // range-selects against (-1 == unset). Reset (selection cleared, anchor
  // unset) whenever entries_ is wholesale replaced by a fresh listing --
  // see do_update_listing(). Mirrors mc.hpp's selected_local_names_/
  // local_selection_anchor_ exactly.
  std::set<std::string> selected_names_;
  int32_t selection_anchor_{-1};

  // Prompt dialog state.
  std::string prompt_root_key_; ///< Empty when no prompt dialog is open.
  bison::key_t prompt_window_id_;
  bison::key_t prompt_input_id_;
  bison::key_t prompt_ok_id_;
  bison::key_t prompt_cancel_id_;
  pending_action prompt_action_{pending_action::none};
  std::vector<std::string> prompt_source_names_; ///< Entries the pending action acts on.
  std::string prompt_value_;                     ///< Live-tracked InputText value.

  /// Overwrite-confirmation dialog (Compress/Extract, second step): a
  /// privately-instantiated MessageBox (see form::instantiate_child_form())
  /// with a "yes_no" preset. Only one may be open at a time; a new
  /// confirmation request just overwrites this member -- the stale
  /// instance's destructor tears down its own internal objects, same effect
  /// the old direct remove_objects_at() call had.
  std::shared_ptr<message_box> confirm_dialog_;

  // View Contents dialog state.
  std::string contents_root_key_; ///< Empty when no contents dialog is open.
  bison::key_t contents_window_id_;
  bison::key_t contents_close_id_;
};

/// @brief Register Zip in the "wish" bison namespace.
void register_zip();

} // namespace bdg::wish
