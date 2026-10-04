// MIT License © 2025 Binary Dice Games
/// @file zip.hpp
/// @brief Server-side form for zip (UI only, no local file access).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace bdg::wish {

class message_box;

/// @brief Client-machine file browser with compress/extract/view-contents
/// actions for zip archives.
///
/// Laid out as three dockable panels inside the tool's own nested
/// dockspace (`dock::viewport()`, see docs/dock-layout.md), like top, pix
/// and the dev modules: **Files** (the form's main root: path bar,
/// selection label, and file table), **Contents** (the listing of the
/// archive last viewed), and **Actions** (Compress/Extract/View
/// Contents/Refresh buttons and the status line). A first-run arrangement
/// is seeded by `on_init()` (Actions as a strip along the top, Files beside
/// Contents below it); the user can re-dock, tab, or float any panel
/// afterwards. Closing any panel closes the whole tool.
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
/// Compress/extract progress is not shown here: the client reports it
/// through the shared ProgressBox dialog
/// (modules/bdg/common/command_worker.hpp), like mc's transfers, and sets
/// `status` to the outcome once the operation is over.
///
/// Emitted events:
///   - `"closed"` — any panel's X button; every panel removed.
///   - `"on_navigate"` (`{name, type}`, `type` is `"dir"` or `"path"`) —
///     client should re-list the target directory and call
///     `update_listing()`.
///   - `"on_compress_requested"` (`{path, source_names, archive_name}`,
///     `source_names` a plain-string array) — client should create
///     `path/archive_name` containing every `path/<name>` in `source_names`
///     (recursively, for a directory), then refresh via `update_listing()`
///     and report the outcome via `set({"status": ...})`.
///   - `"on_extract_requested"` (`{path, zip_name, dest_name}`) — client
///     should extract `path/zip_name` into `path/dest_name`, then refresh
///     and report the outcome the same way.
///   - `"on_view_contents_requested"` (`{path, name}`) — client should read
///     `path/name`'s central directory (without extracting) and call
///     `show_contents(name, entries)`, which fills the Contents panel.
class zip : public form {
 public:
  explicit zip(bison::dynamic&& base);
  /// @brief Removes the secondary Contents/Actions panels; ~form() removes
  /// the main Files panel and the dock layout.
  ~zip() override;

  /// @brief RMI method: replace the browser's displayed directory listing.
  /// @p args holds `path` (string) and `files` (dynamic array of entries,
  /// each `{name, type ("file"/"dir"), size, modified}` — `size`/`modified`
  /// are already client-formatted display strings, mirroring
  /// mc's `update_local_listing()`).
  bison::dynamic do_update_listing(const bison::dynamic& args);

  /// @brief RMI method: show the archive named @p args's `name` in the
  /// Contents panel, replacing whatever archive it showed before. Its table
  /// is populated from @p args's `entries` (dynamic array, each `{name,
  /// type ("file"/"dir"), uncompressed_size, compressed_size}`, as read by
  /// the client from the archive's central directory) and its summary
  /// label names the archive with its entry count and total sizes.
  bison::dynamic do_show_contents(const bison::dynamic& args);

  /// @brief Called from the `__setter` prototype method for every set() call.
  /// Intercepts `status` to mirror it into the internal status label.
  bison::dynamic on_set(const bison::dynamic& patch);

 protected:
  void on_init() override;
  /// @brief Reacts to: `"closed"` (any panel's X button -- emits `"closed"`
  /// and removes every panel); the file table's row selection/activation and
  /// sort events; path_input's `"changed"`; the Actions panel's button
  /// clicks; and the name prompt's events.
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct file_row {
    std::string name;
    std::string type; ///< "file" or "dir"
    std::string size;
    std::string modified;
  };

  /// @brief One entry from an archive's central directory, as reported by
  /// the client for the Contents panel's table.
  struct archive_entry {
    std::string name;
    bool is_dir{false};
    std::uint64_t uncompressed_size{0};
    std::uint64_t compressed_size{0};
  };

  /// @brief What the name/destination prompt dialog, once confirmed, should
  /// do -- mirrors tree.cpp's `pending_transfer`.
  enum class pending_action { none, compress, extract };

  /// @brief Import @p layout_json, assign every element an RMI id, run
  /// @p wire to capture element pointers, and merge the tree under
  /// @p root_key -- registering it as its own top-level object (with
  /// `__path__`, so it can be named in the dock layout) unless it is the
  /// main `internal_root_key_`, which form::init() registers itself.
  void build_window(
      const char* layout_json, const std::string& root_key, bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire);
  /// @brief Remove the Contents/Actions panels and forget their keys. Safe
  /// to call more than once.
  void remove_panel_objects();

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

  // ── Contents panel ────────────────────────────────────────────────────────
  /// @brief Replace the Contents panel's summary and rows with @p zip_name's
  /// @p entries.
  void show_contents_panel(const std::string& zip_name, const std::vector<archive_entry>& entries);

  /// Secondary panel roots: internal_root_key_ + "_contents"/"_actions".
  std::string contents_root_key_;
  std::string actions_root_key_;

  bison::key_t window_id_; ///< Files panel (main root).
  bison::key_t contents_window_id_;
  bison::key_t actions_window_id_;
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
  ui_element_ptr contents_summary_ptr_;
  ui_element_ptr contents_table_ptr_;

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
};

/// @brief Register Zip in the "wish" bison namespace.
void register_zip();

} // namespace bdg::wish
