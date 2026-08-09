// MIT License © 2025 Binary Dice Games
/// @file log_tail.hpp
/// @brief Server-side LogTail form: a `tail`-like log viewer table.
#pragma once

#include "log_line_parser.hpp"

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>

#include <deque>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>

namespace bdg::wish {

/// @brief `tail`-like log viewer: renders a scrolling, auto-following table
/// of log lines pushed by the client, colorized/classified by severity
/// (via `log_line_parser`, configured from the module's `patterns.json`
/// embedded resource), with an optional live regex filter and one extra
/// tab per distinct `[Tag]` token seen in the stream.
///
/// The client owns reading (and, with `-f`, following) local log files --
/// this form never touches the filesystem itself. It only receives
/// already-read raw lines via `push_lines` and parses/renders them, the
/// same server-renders/client-samples split as `ProcessExplorer`. See
/// `modules/bdg/desktop/log_tail/client/log_tail.cpp` for the reference
/// client that implements the actual `tail`-style file reading/following.
///
/// A regex filter set via `set_filter` (or the toolbar's own filter box)
/// applies prospectively only: a line accepted or rejected at the moment it
/// arrives keeps that fate even if the filter is changed afterward --
/// mirroring how `tail -f | grep pattern` only filters output going
/// forward, never rewrites already-printed lines.
class log_tail : public form {
 public:
  explicit log_tail(bison::dynamic&& base);

  /// @brief RMI method: parse and render a batch of raw lines. @p args
  /// holds `lines` (dynamic array), each entry a dynamic with `text`
  /// (string, required) and `source` (string, optional -- shown in the
  /// Source column, e.g. the originating file name).
  bison::dynamic do_push_lines(const bison::dynamic& args);

  /// @brief RMI method: replace the current filter regex and update the
  /// toolbar's filter field to match. @p args holds `pattern` (string; an
  /// empty pattern clears the filter). Applies prospectively only (see
  /// class doc comment). An invalid regex leaves the previous filter
  /// active and reports the error via the status label.
  bison::dynamic do_set_filter(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// One rendered TableRow's bookkeeping: enough to erase it (row + every
  /// cell's RMI object, and its slot in the parent Table's children map)
  /// once it ages out of the FIFO cap.
  struct table_row_entry {
    size_t child_key{0};
    bison::key_t row_id, time_id, level_id, tag_id, source_id, message_id;
  };

  /// One rendered log table (the "All" tab's, or one per discovered tag)
  /// plus its own independent FIFO row cap.
  struct log_table_state {
    ui_element_ptr table_ptr;
    size_t next_child_key{0};
    std::deque<table_row_entry> rows;
  };

  /// One dynamically-created tag tab: its TabItem (for removal on close)
  /// plus its own log_table_state.
  struct tag_tab_state {
    ui_element_ptr tab_ptr;
    bison::key_t tab_id;
    size_t tab_bar_child_key{0};
    log_table_state table;
  };

  /// @brief Parse @p raw (see log_line_parser::parse), and -- if it passes
  /// the current filter -- append a row to the "All" table and, if the
  /// line carries a `[Tag]`, to that tag's own table (creating the tag's
  /// tab on first sighting).
  void ingest_line(const std::string& raw, const std::string& source);

  /// @brief Append one already-parsed line as a new TableRow in @p state,
  /// evicting the oldest row once `kMaxBufferedRows` is exceeded.
  void append_row(log_table_state& state, const parsed_log_line& pl);

  /// @brief Return (creating on first call) the tag_tab_state for @p tag,
  /// building a new closable TabItem + Table inside tab_bar_ptr_.
  tag_tab_state& ensure_tag_tab(const std::string& tag);

  /// @brief Build a 5-column (Time/Level/Tag/Source/Message) Table element,
  /// wired into @p state, but not yet attached to any parent's children.
  ui_element_ptr build_log_table(log_table_state& state);

  /// @brief True if @p raw matches the current filter regex, or if no
  /// filter is active.
  bool passes_filter(const std::string& raw) const;

  /// @brief Re-read the filter box's current `value` field and apply it
  /// via do_set_filter's logic (shared by the Apply button and the filter
  /// InputText's own "changed"/Enter event).
  void apply_filter_from_input();

  /// @brief Remove every row from every table (All + every tag), reset
  /// counters, and update the status label. Does not remove tag tabs
  /// themselves.
  void clear_all();

  /// @brief Erase a tag tab's Table/TabItem objects and forget it (called
  /// when its TabItem's close button is clicked; a later line for the same
  /// tag creates a fresh tab).
  void close_tag_tab(const std::string& tag);

  void update_status();

  log_line_parser parser_;

  bison::key_t window_id_;
  ui_element_ptr filter_input_ptr_;
  bison::key_t filter_input_id_;
  bison::key_t btn_apply_filter_id_;
  bison::key_t btn_clear_filter_id_;
  bison::key_t btn_clear_id_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr tab_bar_ptr_;
  size_t next_tab_child_key_{0};
  size_t next_table_seq_{0}; ///< Monotonic counter for unique per-table ImGui string ids.

  log_table_state all_table_;
  std::unordered_map<std::string, tag_tab_state> tag_tabs_; ///< keyed by tag text
  std::unordered_map<uint32_t, std::string> tab_id_to_tag_;

  std::optional<std::regex> filter_regex_;
  std::string filter_pattern_;
  uint64_t total_lines_received_{0};
};

/// @brief Register LogTail in the "wish" bison namespace.
void register_log_tail();

} // namespace bdg::wish
