// MIT License © 2025 Binary Dice Games
/// @file nano.hpp
/// @brief Server-side form for nano (a multi-file text editor).
#pragma once

#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Multi-file, syntax-highlighted text editor form.
///
/// Laid out as dockable panels inside the editor's own nested dockspace
/// (`dock::viewport()`, see docs/dock-layout.md), like top, pix and the dev
/// modules:
///   - **Toolbar** (the form's main root): Open / New / Save / Find buttons
///     and the name of the current document. Its X closes the editor.
///   - **One window per open file** (a language `Combo` above a
///     `TextEditor`), titled with the file's name. Each opens into the
///     dockspace's central "documents" area (`Window.dock_target`), tabbed
///     with the other files, and can then be re-docked side by side, split,
///     or floated like any other panel. Its X closes that file.
///   - **Search**: a Notepad++-style Find panel -- "Find what", "Match
///     whole word only", "Match case", Search Mode (Normal / Extended /
///     Regular expression), "Find All in Current Document", "Find All in All
///     Opened Documents", "Count" -- with a results table (File, Line,
///     Text) under a `Search "x" (N hits in M files of K searched)` summary.
///     Clicking a result brings that file's window to the front and selects
///     the match. Its X only hides it; the toolbar's Find shows it again.
///
/// The **current document** -- the one Save, "Find All in Current Document"
/// and Count act on -- is the file window that last gained focus (the
/// Window `"focused"` event), or the most recently opened one.
///
/// Files edited by this form live in the session's sandboxed resource
/// directory (`context::resource_dir`); the connected client is responsible
/// for moving bytes into and out of that sandbox via `client::upload_file` /
/// `client::download_file`. Opening a file is therefore a two-step
/// handshake: the client uploads the file's contents, then calls the
/// `open_file` method with the resulting sandbox-relative path. The
/// `TextEditor` reads and writes the sandbox file directly (every edit is
/// written through), which is also what Search reads -- so it always sees
/// the live, unsaved text. The language combo is seeded from the file's
/// extension (see `language_for_extension()` in nano.cpp) but the user may
/// override it at any time; doing so does not mark the file dirty.
///
/// Clicking "Open" emits `on_request_open` so the client can present its
/// own file picker and upload the chosen file before calling `open_file`;
/// "New" emits `on_request_new` (same handshake, for a fresh file). "Save"
/// (or Ctrl+S in an editor) emits `on_file_saved` for that one file, asking
/// the client to download and persist it. Closing a file window, or the
/// whole editor, emits `on_file_closed` per file so the client can download
/// it one last time before discarding its local bookkeeping.
///
/// Each open file tracks whether it has unsaved changes: its window title
/// gets a `" *"` suffix while its `TextEditor` has been edited since the
/// last save. If the editor is closed (the Toolbar's X) while any file is
/// still unsaved, `on_confirm_close` is emitted instead (payload: `paths`,
/// the unsaved files) so the client can ask the user. `confirm_close(save)`
/// finishes the close: `save: true` flushes every open file; `save: false`
/// flushes only the already-saved ones. A canceled close simply never calls
/// `confirm_close`, leaving every panel open as it was.
class nano : public common::tool_form {
 public:
  explicit nano(bison::dynamic&& base);
  /// @brief Removes the Search panel and every file window; ~form() removes
  /// the Toolbar and the dock layout.
  ~nano() override;

  /// @brief RMI method: register an already-uploaded sandbox file as a new
  /// file window. @p args holds `path` (string, required, sandbox-relative)
  /// and an optional `title` (string; defaults to `path`). The new file
  /// becomes the current document. Emits `on_file_opened` on success or
  /// `on_error` if the path is empty or escapes the sandbox. If `path` is
  /// already open, its window is brought to the front instead.
  bison::dynamic do_open_file(const bison::dynamic& args);

  /// @brief RMI method: the user's answer to the `on_confirm_close` prompt.
  /// @p args holds `save` (bool). Completes the close that was deferred
  /// when it was emitted -- see the class doc comment above.
  bison::dynamic do_confirm_close(const bison::dynamic& args);

  /// @brief RMI method: run a search as if typed into the Search panel and
  /// return the hit count. @p args holds `text` (string), and optionally
  /// `mode` (`"normal"` (default), `"extended"` or `"regex"`), `match_case`
  /// and `whole_word` (bools, default false) and `scope` (`"current"`
  /// (default) or `"all"`). Updates the panel's fields, summary and results
  /// exactly as its buttons do. Returns `{hits: int}`.
  bison::dynamic do_find(const bison::dynamic& args);

 protected:
  void on_init() override;
  /// @brief Reacts to: the Toolbar's X (close the editor, possibly via
  /// `on_confirm_close`) and buttons; a file window's X (`"closed"`, close
  /// that file), `"focused"` (make it current), its editor's `"changed"`/
  /// `"saved"` and its language combo; the Search panel's X (hide), option
  /// controls, buttons, and a results row click (`"row_selected"`).
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  struct open_file_entry {
    std::string path;            ///< sandbox-relative path
    std::string full_path;       ///< resolved on-disk path, read by Search
    std::string title;           ///< window title, without the unsaved suffix
    std::string root_key;        ///< this file window's top-level key
    bison::key_t window_id;      ///< Window __wish_id
    bison::key_t editor_id;      ///< TextEditor __wish_id
    bison::key_t lang_combo_id;  ///< language Combo __wish_id
    ui_element_ptr window_ptr;   ///< to retitle / focus it
    ui_element_ptr editor_ptr;   ///< to retarget "language" and issue go-to requests
    bool dirty{false};           ///< true once edited since the last save
  };

  /// Search Mode radio choice -- Notepad++'s three modes.
  enum class search_mode { normal, extended, regex };

  /// One results-table row: the first match on one line of one file.
  struct search_hit {
    std::string path;     ///< sandbox path of the file it is in
    int32_t line{0};      ///< 1-based
    int32_t column{0};    ///< 0-based, in characters (UTF-8 code points)
    int32_t length{0};    ///< match length in characters, clamped to the line
  };

  /// @brief Remove the Search panel and every file window. Safe to call
  /// more than once.
  void remove_panel_objects();

  /// @brief Remove the file window at @p index from the UI and tracking
  /// state, then emit `on_file_closed` for it.
  void close_file_at(size_t index);

  /// @brief Index into open_files_ of the file whose window, editor or
  /// language combo has @p id, or -1.
  int find_file_by_widget(bison::key_t id) const;
  int find_file_by_path(const std::string& path) const;

  /// @brief Refresh the file window title at @p index from its title/dirty
  /// state.
  void update_window_title(size_t index);

  /// @brief Make @p index (or none, when -1) the current document and
  /// update the Toolbar's "current" label.
  void set_current(int index);

  /// @brief Bring the file window at @p index to the front.
  void focus_file(size_t index);

  /// @brief Emit `on_file_saved` for @p index and clear its dirty mark.
  void save_file_at(size_t index);

  /// @brief Flush and tear down every open file, then emit `"closed"` and
  /// remove every panel. @p flush_dirty_files selects whether a still-
  /// unsaved file is flushed (downloaded by the client) too, or skipped so
  /// its local copy is left untouched.
  void do_close(bool flush_dirty_files);

  // ── Search ─────────────────────────────────────────────────────────────────

  /// @brief Run the search described by the Search panel's current fields
  /// over the current document (@p all_files false) or every open file,
  /// fill the results table and summary. @p count_only only reports the
  /// number of matches in the current document, leaving the results alone.
  /// @return The number of matches found (0 on error).
  int run_search(bool all_files, bool count_only);

  /// @brief Show the Search panel again (after its X hid it) and focus it.
  void show_search_panel();

  /// @brief Move the go-to selection of the hit at results row @p row into
  /// its file's editor and bring that file to the front.
  void go_to_hit(size_t row);

  /// @brief Set the Search Mode radios to @p mode.
  void set_search_mode(search_mode mode);

  /// @brief Replace the results table's rows with search_hits_.
  void fill_results_table();

  ui_element_ptr find_input_ptr_;
  ui_element_ptr chk_whole_word_ptr_;
  ui_element_ptr chk_match_case_ptr_;
  ui_element_ptr radio_normal_ptr_;
  ui_element_ptr radio_extended_ptr_;
  ui_element_ptr radio_regex_ptr_;
  ui_element_ptr summary_label_ptr_;
  ui_element_ptr results_table_ptr_;
  ui_element_ptr search_window_ptr_;
  ui_element_ptr current_label_ptr_;

  std::string search_root_key_; ///< internal_root_key_ + "_search"
  size_t next_doc_seq_{0};       ///< suffix of the next file window's root key

  bison::key_t window_id_;  ///< Toolbar (main root)
  bison::key_t search_window_id_;
  bison::key_t btn_open_id_;
  bison::key_t btn_new_id_;
  bison::key_t btn_save_id_;
  bison::key_t btn_find_id_;
  bison::key_t find_input_id_;
  bison::key_t chk_whole_word_id_;
  bison::key_t chk_match_case_id_;
  bison::key_t radio_normal_id_;
  bison::key_t radio_extended_id_;
  bison::key_t radio_regex_id_;
  bison::key_t btn_find_current_id_;
  bison::key_t btn_find_all_id_;
  bison::key_t btn_count_id_;
  bison::key_t btn_clear_id_;
  bison::key_t results_table_id_;

  std::vector<open_file_entry> open_files_;
  int current_index_{-1}; ///< open_files_ index of the current document, or -1

  search_mode mode_{search_mode::normal};
  std::vector<search_hit> search_hits_;
};

/// @brief Register Nano in the "wish" bison namespace.
void register_nano();

} // namespace bdg::wish
