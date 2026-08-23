// MIT License © 2025 Binary Dice Games
/// @file tail.hpp
/// @brief Server-side form for tail (a `tail`-like log viewer table).
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

/// @brief `tail`-like log viewer: renders a scrolling table of log lines
/// pushed by the client, colorized/classified by severity (via
/// `log_line_parser`, configured from the module's `patterns.json`
/// embedded resource), with an optional live regex filter, a toggleable
/// "Follow" auto-scroll, and one extra tab per distinct `[Tag]` token seen
/// in the stream.
///
/// The client owns reading (and, with `-f`, following) local log files --
/// this form never touches the filesystem itself. It only receives
/// already-read raw lines via `push_lines` and parses/renders them, the
/// same server-renders/client-samples split as `Top`. See
/// `modules/bdg/desktop/tail/client/tail.cpp` for the reference
/// client that implements the actual `tail`-style file reading/following.
///
/// A regex filter set via `set_filter` (or the toolbar's own filter box)
/// controls row *visibility*, not admission: every ingested line always
/// becomes a row (up to `kMaxBufferedRows`), and the filter only decides
/// which of the already-buffered rows are currently shown. Changing the
/// pattern re-walks every buffered row and updates its visibility on the
/// spot, so the filter can be edited dynamically to search through lines
/// already on screen -- unlike `tail -f | grep pattern`, which only ever
/// filters output going forward.
///
/// The toolbar's Lines field is a *live* cap on every table's row count
/// (default matches whatever the client's own `-n` used at startup, see
/// `do_set_line_count`): whenever a table would hold more rows than this,
/// the oldest are dropped, whether that growth came from a newly ingested
/// line or from the user lowering the field itself (which evicts down to
/// the new cap immediately). This form has no filesystem access, so it
/// cannot on its own pull in more history when the user *raises* the
/// field -- editing it from the UI additionally clears every table and
/// emits 'rescan_requested' with {line_count: int}, which the client is
/// expected to answer by re-reading each tailed file's last N lines and
/// push_lines-ing them back in (see
/// `modules/bdg/desktop/tail/client/tail.cpp`'s own handler) -- so raising
/// the count really does surface more history, not just relax future
/// eviction.
class tail : public form {
 public:
  explicit tail(bison::dynamic&& base);

  /// @brief RMI method: parse and render a batch of raw lines. @p args
  /// holds `lines` (dynamic array), each entry a dynamic with `text`
  /// (string, required) and `source` (string, optional -- shown in the
  /// Source column, e.g. the originating file name).
  bison::dynamic do_push_lines(const bison::dynamic& args);

  /// @brief RMI method: replace the current filter regex, update the
  /// toolbar's filter field to match, and re-evaluate every already-
  /// buffered row's visibility against the new pattern (see class doc
  /// comment). @p args holds `pattern` (string; an empty pattern clears
  /// the filter, making every row visible). An invalid regex leaves the
  /// previous filter (and every row's current visibility) unchanged and
  /// reports the error via the status label.
  bison::dynamic do_set_filter(const bison::dynamic& args);

  /// @brief RMI method: set the toolbar's Lines field and the live row cap
  /// it represents (see class doc comment). @p args holds `count`, clamped
  /// to `[1, kMaxBufferedRows]`; returns `{count: int}`, the value actually
  /// applied after clamping. Immediately evicts the oldest row(s) from
  /// every table (the "All" tab's and every tag tab's) that currently
  /// exceeds the new cap -- but never pulls in *more* rows, since this
  /// form has no filesystem access; only on_event()'s handling of the
  /// field's own "changed" event does that, via 'rescan_requested' (see
  /// class doc comment). Used both by the client to reflect its actual
  /// startup `-n` value (no eviction expected there, but harmless if the
  /// initial batch somehow already exceeded it) and, as a first step, by
  /// that on_event() handler.
  bison::dynamic do_set_line_count(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// One rendered TableRow's bookkeeping: enough to erase it (row + every
  /// cell's RMI object, and its slot in the parent Table's children map)
  /// once it ages out of the FIFO cap, and enough to re-evaluate its
  /// visibility against a newly changed filter (`raw`).
  struct table_row_entry {
    size_t child_key{0};
    bison::key_t row_id, time_id, level_id, tag_id, source_id, message_id;
    std::string raw; ///< The original unparsed line, matched against filter_regex_.
  };

  /// One rendered log table (the "All" tab's, or one per discovered tag)
  /// plus its own independent FIFO row cap.
  struct log_table_state {
    ui_element_ptr table_ptr;
    size_t next_child_key{0};
    std::deque<table_row_entry> rows;
  };

  /// One dynamically-created tag tab: its TabItem (not user-closable --
  /// see ensure_tag_tab()) plus its own log_table_state.
  struct tag_tab_state {
    ui_element_ptr tab_ptr;
    log_table_state table;
  };

  /// @brief Parse @p raw (see log_line_parser::parse) and unconditionally
  /// append a row to the "All" table and, if the line carries a `[Tag]`, to
  /// that tag's own table (creating the tag's tab on first sighting) --
  /// every ingested line becomes a row regardless of the current filter,
  /// which only governs visibility (see class doc comment).
  void ingest_line(const std::string& raw, const std::string& source);

  /// @brief Append one already-parsed line as a new TableRow in @p state,
  /// then evict_to_cap()s it. The new row's initial `visible` field is set
  /// from `passes_filter(raw)`.
  void append_row(log_table_state& state, const parsed_log_line& pl, const std::string& raw);

  /// @brief Drop the oldest row(s) from @p state until its row count is at
  /// most `max_rows_` (see class doc comment) -- a no-op if it's already
  /// within cap. Erases each dropped row's cells + row RMI objects and its
  /// slot in the parent Table's children map, but does not itself call
  /// `refresh_children_order()`; callers that mutated the same table's
  /// children map already do (append_row()'s own trailing call; the ones
  /// do_set_line_count() adds per table it touches).
  void evict_to_cap(log_table_state& state);

  /// @brief Return (creating on first call) the tag_tab_state for @p tag,
  /// building a new (non-closable, matching the "All" tab) TabItem + Table
  /// inside tab_bar_ptr_. Once created, a tag's tab persists for the rest
  /// of the session -- there is no user-facing way to remove it.
  tag_tab_state& ensure_tag_tab(const std::string& tag);

  /// @brief Build a 5-column (Time/Level/Tag/Source/Message) Table element,
  /// wired into @p state, but not yet attached to any parent's children.
  ui_element_ptr build_log_table(log_table_state& state);

  /// @brief True if @p raw matches the current filter regex, or if no
  /// filter is active.
  bool passes_filter(const std::string& raw) const;

  /// @brief Re-read the filter box's current `value` field and apply it
  /// via do_set_filter's logic (called on every filter InputText "changed"
  /// event, i.e. every keystroke).
  void apply_filter_from_input();

  /// @brief Re-evaluate every already-buffered row's `visible` field
  /// against the current filter (`passes_filter(entry.raw)`), across the
  /// "All" table and every tag tab's table. Called whenever the filter
  /// pattern changes so previously ingested lines' visibility catches up
  /// immediately, without needing new lines to arrive.
  void reapply_filter_visibility();

  /// @brief Push the current `follow_enabled_` value onto every existing
  /// table's `auto_scroll` field (the "All" tab's and every tag tab's), so
  /// the Follow checkbox's state governs whether render_table() sticks to
  /// the newest row as lines are appended.
  void apply_follow_state();

  /// @brief Remove every row from every table (All + every tag), reset
  /// counters, and update the status label. Does not remove tag tabs
  /// themselves.
  void clear_all();

  void update_status();

  log_line_parser parser_;

  bison::key_t window_id_;
  ui_element_ptr filter_input_ptr_;
  bison::key_t filter_input_id_;
  ui_element_ptr lines_input_ptr_;
  bison::key_t lines_input_id_;
  bison::key_t follow_checkbox_id_;
  bison::key_t btn_clear_id_;
  ui_element_ptr status_label_ptr_;
  ui_element_ptr tab_bar_ptr_;
  size_t next_tab_child_key_{0};
  size_t next_table_seq_{0}; ///< Monotonic counter for unique per-table ImGui string ids.

  log_table_state all_table_;
  std::unordered_map<std::string, tag_tab_state> tag_tabs_; ///< keyed by tag text

  std::optional<std::regex> filter_regex_;
  std::string filter_pattern_;
  uint64_t total_lines_received_{0};

  /// Mirrors the toolbar's Follow checkbox: true (default) keeps every
  /// table's `auto_scroll` field on, so render_table() sticks to the
  /// newest row as lines are appended; false leaves scroll position alone.
  bool follow_enabled_{true};

  /// Mirrors the toolbar's Lines field (default matches its own "value" in
  /// kLayout): the live per-table row cap enforced by append_row()/
  /// evict_to_cap(), clamped to `[1, kMaxBufferedRows]` by do_set_line_count().
  size_t max_rows_{10};
};

/// @brief Register Tail in the "wish" bison namespace.
void register_tail();

} // namespace bdg::wish
