// MIT License © 2025 Binary Dice Games
/// @file tail.cpp
/// @brief Implementation of the Tail form.
#include "tail.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <cctype>
#include <fstream>
#include <sstream>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

std::string to_upper(const std::string& s) {
  std::string out = s;
  for (auto& c : out)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

/// Absolute ceiling on tail::max_rows_ (the toolbar Lines field's live
/// per-table row cap, oldest evicted first): a live tail could otherwise
/// run for hours and, if the user sets an extreme Lines value, accumulate
/// an unbounded RMI object count.
constexpr size_t kMaxBufferedRows = 2000;

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// The filter box has no Apply/Clear buttons: it fires "changed" on every
// keystroke (plain InputText, no EnterReturnsTrue flag), and on_event()
// re-applies the filter from its current value on each one -- an empty
// value clears the filter the same way (see do_set_filter's
// pattern.empty() branch). Filtering only toggles row visibility (see
// class doc comment), so a "changed" per keystroke costs one regex compile
// plus one `visible` write per already-buffered row (reapply_filter_
// visibility()), not a re-render of the whole table.
//
// "lines_input"'s "step": 0/"step_fast": 0 hides InputInt's +/- spinner
// buttons (see ImGui::InputInt()'s own `step > 0 ? &step : NULL`), leaving
// a plain text box that fills its whole widget rect -- confirmed live via
// the automation module that with the buttons shown, a plain center-click
// (what both a real user aiming at a narrow control and
// AutomationClient.click()/type_text() do) lands on the "-"/"+" pair
// rather than the much narrower text portion, since they visually
// dominate an 80px-wide control. "EnterReturnsTrue" (unlike the filter
// box) is deliberate here for a stronger reason than the filter's own
// "avoid wasted work": changing Lines *evicts* rows down to the new cap
// (see do_set_line_count()), which -- unlike the filter's reversible
// visibility toggle -- is destructive and unrecoverable. Firing that per
// keystroke while typing a multi-digit number (e.g. hitting "5" on the
// way to "50") would irreversibly drop rows a moment before the field
// settles on a larger value that would have kept them.
// ImGuiTableColumnFlags: WidthFixed=16, WidthStretch=8 (message column
// fills whatever width remains, matching kHelpWindowLayout's col_desc in
// modules/bdg/dev/editor/server/editor.cpp).
// ImGuiTableFlags: Resizable(1) + RowBg(64) + Borders(1920) +
// ScrollY(1<<25=33554432) = 33556417 -- see modules/bdg/desktop/git/server/git.cpp's
// identical combination; ScrollY both clips the table to outer_height and
// (via render_table()'s own auto-scroll logic) keeps the newest row in view.
// "tab_bar"'s own "height": -1 is load-bearing, not cosmetic: table_all's
// "outer_height": -1 asks to fill whatever ambient region it's rendered
// into, but tab_bar has no explicit Layout height hint of its own (the
// default is 0, "auto -- use my own measured natural size"), so without
// this, arrange_vertical_layout() would treat tab_bar as an auto child and
// hand it back its own *previous frame's real rendered height* (via
// measure_node()'s last_rendered_size() fallback, see imgui_layout.cpp) --
// which already includes table_all's own fill-driven height, compounding
// without bound frame over frame (confirmed via WISH_LAYOUT_DEBUG_LOG: the
// whole vbox/tab_bar/table_all chain grew in lockstep, +250px per rendered
// frame, with no ceiling). Marking tab_bar itself as a stretch (-1) child
// makes its size a deterministic share of vbox's own real avail instead,
// breaking the cycle -- see imgui_ui_renderer.cpp's render_vertical_layout()
// for how a nonzero height hint gets a real bounding BeginChild() wrap.
static constexpr const char* kLayout = R"json({
  "type": "Window",
  "title": "Tail",
  "width": 960, "height": 620,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "filter_input": { "type": "InputText", "label": "Filter (regex)", "hint": "e.g. error|timeout", "width": 320 },
            "lines_input": { "type": "InputInt", "label": "Lines", "value": 10, "step": 0, "step_fast": 0, "width": 80, "flags": "EnterReturnsTrue" },
            "chk_follow": { "type": "Checkbox", "label": "Follow", "value": true },
            "btn_clear": { "type": "Button", "label": "Clear All" }
          }
        },
        "status_label": { "type": "Label", "text": "0 lines" },
        "tab_bar": {
          "type": "TabBar", "id": "##tail_tabs", "height": -1,
          "children": {
            "tab_all": {
              "type": "TabItem", "label": "All", "closable": false,
              "children": {
                "table_all": {
                  "type": "Table", "id": "##tail_all", "columns": 5, "headers": true,
                  "outer_height": -1, "flags": "Resizable|RowBg|Borders|ScrollY", "auto_scroll": true,
                  "children": {
                    "col_time":    { "type": "TableColumn", "label": "Time",    "column_id": 0, "flags": "WidthFixed", "init_width": 90 },
                    "col_level":   { "type": "TableColumn", "label": "Level",   "column_id": 1, "flags": "WidthFixed", "init_width": 70 },
                    "col_tag":     { "type": "TableColumn", "label": "Tag",     "column_id": 2, "flags": "WidthFixed", "init_width": 100 },
                    "col_source":  { "type": "TableColumn", "label": "Source",  "column_id": 3, "flags": "WidthFixed", "init_width": 120 },
                    "col_message": { "type": "TableColumn", "label": "Message", "column_id": 4, "flags": "WidthStretch" }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
})json";

// ── tail ─────────────────────────────────────────────────────────────────

tail::tail(dynamic&& base) : form(std::move(base)) {}

void tail::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__tail_");

  auto tree = import_json(kLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Tail"};

  // put_object() files each element under the current request's group (see
  // rmi::context::current_group) so they're cleaned up together with the
  // rest of this form when relayed through rmi::bridge.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.toolbar.filter_input", [&](const auto& e) {
    filter_input_ptr_ = e;
    filter_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.toolbar.lines_input", [&](const auto& e) {
    lines_input_ptr_ = e;
    lines_input_id_ = wish_id_of(e);
  });
  tree.with("vbox.toolbar.chk_follow", [&](const auto& e) { follow_checkbox_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_clear", [&](const auto& e) { btn_clear_id_ = wish_id_of(e); });
  tree.with("vbox.status_label", [&](const auto& e) { status_label_ptr_ = e; });
  tree.with("vbox.tab_bar", [&](const auto& e) { tab_bar_ptr_ = e; });
  tree.with("vbox.tab_bar.tab_all.table_all", [&](const auto& e) { all_table_.table_ptr = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);

  next_table_seq_ = 1; // 0 is implicitly "reserved" for the "All" tab's static table.

  // Load the module's patterns.json config resource (see modules/README.md's
  // "Per-module embedded resources"): extracted per-session at
  // res/bdg/desktop/tail/patterns.json. Never fails hard -- on any
  // error, log_line_parser keeps its previous (or built-in minimal
  // fallback) rules; see log_line_parser::load_from_json()'s doc comment.
  auto resolved = file_service::resolve_path(
      "res/bdg/desktop/tail/patterns.json", sess().resource_dir, sess().allow_absolute_paths);
  if (!resolved.empty()) {
    std::ifstream in(resolved, std::ios::binary);
    if (in) {
      std::string contents{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
      parser_.load_from_json(contents);
    }
  }

  apply_follow_state();
  update_status();
}

// ── push_lines / set_filter ───────────────────────────────────────────────────

dynamic tail::do_push_lines(const dynamic& args) {
  auto* lines_f = args.findField<dynamic_ptr>("lines"_key);
  if (!lines_f || !*lines_f)
    return dynamic{};

  (*lines_f)->forEach([&](key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto entry_ptr = f.as<dynamic_ptr>();
    if (!entry_ptr)
      return;
    auto& e = *entry_ptr;

    std::string text = e.as<std::string>("text"_key);
    std::string source;
    if (auto* s = e.findField<std::string>("source"_key))
      source = *s;
    ingest_line(text, source);
  });

  return dynamic{};
}

dynamic tail::do_set_filter(const dynamic& args) {
  std::string pattern;
  if (auto* p = args.findField<std::string>("pattern"_key))
    pattern = *p;

  if (pattern.empty()) {
    filter_regex_.reset();
    filter_pattern_.clear();
  } else {
    // Construct into a local first: std::optional::emplace() would destroy
    // the previously-active filter before the new one is known to be valid,
    // breaking the "invalid regex leaves the previous filter active"
    // contract documented on do_set_filter().
    try {
      std::regex new_regex(pattern, std::regex::icase);
      filter_regex_ = std::move(new_regex);
      filter_pattern_ = pattern;
    } catch (const std::regex_error& e) {
      if (status_label_ptr_)
        status_label_ptr_["text"_key] = std::string{"Invalid filter regex: "} + e.what();
      return dynamic{};
    }
  }

  if (filter_input_ptr_)
    filter_input_ptr_["value"_key] = pattern;
  reapply_filter_visibility();
  update_status();
  return dynamic{};
}

dynamic tail::do_set_line_count(const dynamic& args) {
  int32_t count = static_cast<int32_t>(max_rows_);
  if (auto* c = args.findField<int32_t>("count"_key))
    count = *c;
  if (count < 1)
    count = 1;
  else if (count > static_cast<int32_t>(kMaxBufferedRows))
    count = static_cast<int32_t>(kMaxBufferedRows);

  max_rows_ = static_cast<size_t>(count);
  if (lines_input_ptr_)
    lines_input_ptr_["value"_key] = count;

  evict_to_cap(all_table_);
  if (all_table_.table_ptr)
    all_table_.table_ptr->refresh_children_order();
  for (auto& [tag, state] : tag_tabs_) {
    evict_to_cap(state.table);
    if (state.table.table_ptr)
      state.table.table_ptr->refresh_children_order();
  }

  dynamic result;
  result["count"_key] = count;
  return result;
}

// ── Line ingestion / rendering ────────────────────────────────────────────────

void tail::ingest_line(const std::string& raw, const std::string& source) {
  parsed_log_line pl = parser_.parse(raw, source);
  ++total_lines_received_;

  append_row(all_table_, pl, raw);
  if (!pl.tag.empty())
    append_row(ensure_tag_tab(pl.tag).table, pl, raw);

  update_status();
}

bool tail::passes_filter(const std::string& raw) const {
  if (!filter_regex_)
    return true;
  return std::regex_search(raw, *filter_regex_);
}

void tail::append_row(log_table_state& state, const parsed_log_line& pl, const std::string& raw) {
  if (!state.table_ptr)
    return;
  auto* children_p = state.table_ptr->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  // Both colors pl carries for a classified line are stored on the cell,
  // undecided -- render_label() (imgui_ui_renderer.cpp) picks between
  // "text_color_light"/"text_color_dark" at render time, live against the
  // session's actual active theme every frame, the same way
  // render_text_editor() picks the Nano TextEditor's syntax palette. This
  // form never touches style_service at all: baking in one color here,
  // resolved once at ingest time, would leave already-ingested lines stuck
  // with whatever theme was active when each line first arrived instead of
  // following a theme change made later in the same session.
  auto make_cell = [&](const std::string& text, const std::string& light_color, const std::string& dark_color,
                        int32_t order) {
    ui_element_ptr cell = ui_element_ptr::create("wish"_key, "Label"_key);
    cell["text"_key] = text;
    if (!light_color.empty())
      cell["text_color_light"_key] = light_color;
    if (!dark_color.empty())
      cell["text_color_dark"_key] = dark_color;
    cell["order"_key] = order;
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, cell);
    cell["__wish_id"_key] = id;
    return cell;
  };

  std::string level_display = pl.level.empty() ? std::string{"-"} : to_upper(pl.level);
  ui_element_ptr time_cell = make_cell(pl.timestamp, "", "", 0);
  ui_element_ptr level_cell = make_cell(level_display, pl.light_color, pl.dark_color, 1);
  ui_element_ptr tag_cell = make_cell(pl.tag, "", "", 2);
  ui_element_ptr source_cell = make_cell(pl.source, "", "", 3);
  ui_element_ptr message_cell = make_cell(pl.message, pl.light_color, pl.dark_color, 4);

  ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
  row["order"_key] = static_cast<int32_t>(state.next_child_key);
  // Filtering hides rows rather than gating admission (see class doc
  // comment) -- render_table() skips a row entirely when its inherited
  // "visible" field is false (see docs/ui-elements.md's TableRow section).
  row["visible"_key] = passes_filter(raw);

  auto row_children = dynamic_ptr{key_t{0U}, {}};
  (*row_children)[size_t{0}] = dynamic_ptr{time_cell};
  (*row_children)[size_t{1}] = dynamic_ptr{level_cell};
  (*row_children)[size_t{2}] = dynamic_ptr{tag_cell};
  (*row_children)[size_t{3}] = dynamic_ptr{source_cell};
  (*row_children)[size_t{4}] = dynamic_ptr{message_cell};
  row["children"_key] = row_children;
  row->refresh_children_order();

  key_t row_id = rmi::shared::generate_id();
  ctx().put_object(row_id, row);
  row["__wish_id"_key] = row_id;

  size_t child_key = state.next_child_key++;
  (*children)[child_key] = dynamic_ptr{row};
  state.rows.push_back({child_key, row_id, wish_id_of(time_cell), wish_id_of(level_cell), wish_id_of(tag_cell),
      wish_id_of(source_cell), wish_id_of(message_cell), raw});

  // Live FIFO cap: the toolbar's Lines field (see class doc comment) --
  // a live tail has no natural upper bound on how many lines it will ever
  // see, so the oldest row is dropped once max_rows_ is exceeded.
  evict_to_cap(state);

  state.table_ptr->refresh_children_order();
}

void tail::evict_to_cap(log_table_state& state) {
  if (!state.table_ptr)
    return;
  auto* children_p = state.table_ptr->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  while (state.rows.size() > max_rows_) {
    auto& oldest = state.rows.front();
    children->erase(oldest.child_key);
    ctx().objects.erase(oldest.row_id.id);
    ctx().objects.erase(oldest.time_id.id);
    ctx().objects.erase(oldest.level_id.id);
    ctx().objects.erase(oldest.tag_id.id);
    ctx().objects.erase(oldest.source_id.id);
    ctx().objects.erase(oldest.message_id.id);
    state.rows.pop_front();
  }
}

// ── Tag tabs ───────────────────────────────────────────────────────────────────

ui_element_ptr tail::build_log_table(log_table_state& state) {
  ui_element_ptr table = ui_element_ptr::create("wish"_key, "Table"_key);
  table["id"_key] = "##tail_tbl_" + std::to_string(next_table_seq_++);
  table["columns"_key] = int32_t{5};
  table["headers"_key] = true;
  table["outer_height"_key] = -1.0f;
  table["flags"_key] = int32_t{33556417}; // Resizable|RowBg|Borders|ScrollY -- see kLayout's comment.
  table["auto_scroll"_key] = follow_enabled_;

  auto make_col = [&](const char* label, int32_t col_id, int32_t flags, float w, int32_t order) {
    ui_element_ptr col = ui_element_ptr::create("wish"_key, "TableColumn"_key);
    col["label"_key] = std::string{label};
    col["column_id"_key] = col_id;
    col["flags"_key] = flags;
    col["init_width"_key] = w;
    col["order"_key] = order;
    key_t id = rmi::shared::generate_id();
    ctx().put_object(id, col);
    col["__wish_id"_key] = id;
    return col;
  };

  // Named (string-hash) keys, not numeric 0..4: this table's "children" map
  // later also holds TableRow entries added by append_row() via
  // log_table_state::next_child_key, which counts up from a *numeric* 0 --
  // using plain size_t{0}..{4} here for the columns would collide with the
  // first few rows appended (dynamic's operator[](size_t) and
  // operator[](key_t) share the same underlying field map, see
  // bison_object.hpp), silently overwriting each TableColumn with a
  // TableRow and losing both the header labels and the column widths.
  // Mirrors kLayout's own JSON child names (col_time, col_level, ...),
  // whose hashed string keys are subject to the same rule but never
  // collide with a small numeric row index in practice.
  auto children = dynamic_ptr{key_t{0U}, {}};
  (*children)["col_time"_key] = dynamic_ptr{make_col("Time", 0, 16, 90.0f, 0)};
  (*children)["col_level"_key] = dynamic_ptr{make_col("Level", 1, 16, 70.0f, 1)};
  (*children)["col_tag"_key] = dynamic_ptr{make_col("Tag", 2, 16, 100.0f, 2)};
  (*children)["col_source"_key] = dynamic_ptr{make_col("Source", 3, 16, 120.0f, 3)};
  (*children)["col_message"_key] = dynamic_ptr{make_col("Message", 4, 8, 0.0f, 4)};
  table["children"_key] = children;
  table->refresh_children_order();

  key_t table_id = rmi::shared::generate_id();
  ctx().put_object(table_id, table);
  table["__wish_id"_key] = table_id;

  state.table_ptr = table;
  return table;
}

tail::tag_tab_state& tail::ensure_tag_tab(const std::string& tag) {
  if (auto it = tag_tabs_.find(tag); it != tag_tabs_.end())
    return it->second;

  tag_tab_state state;
  ui_element_ptr table = build_log_table(state.table);

  ui_element_ptr tab = ui_element_ptr::create("wish"_key, "TabItem"_key);
  tab["label"_key] = "[" + tag + "]";
  // Not closable, matching the "All" tab (see kLayout's own "closable":
  // false) -- a tag's tab is a permanent part of the session's tab bar
  // once created, the same way "All" is; the user cannot remove either.
  tab["closable"_key] = false;
  tab["order"_key] = static_cast<int32_t>(next_tab_child_key_ + 1);

  auto tab_children = dynamic_ptr{key_t{0U}, {}};
  (*tab_children)[size_t{0}] = dynamic_ptr{table};
  tab["children"_key] = tab_children;
  tab->refresh_children_order();

  key_t tab_id = rmi::shared::generate_id();
  ctx().put_object(tab_id, tab);
  tab["__wish_id"_key] = tab_id;

  if (tab_bar_ptr_) {
    if (auto* children_p = tab_bar_ptr_->findField<dynamic_ptr>("children"_key); children_p && *children_p) {
      size_t child_key = next_tab_child_key_++;
      (*(*children_p))[child_key] = dynamic_ptr{tab};
      tab_bar_ptr_->refresh_children_order();
    }
  }

  state.tab_ptr = tab;

  auto [ins_it, ok] = tag_tabs_.emplace(tag, std::move(state));
  return ins_it->second;
}

// ── Toolbar actions ───────────────────────────────────────────────────────────

void tail::apply_follow_state() {
  if (all_table_.table_ptr)
    all_table_.table_ptr["auto_scroll"_key] = follow_enabled_;
  for (auto& [tag, state] : tag_tabs_) {
    if (state.table.table_ptr)
      state.table.table_ptr["auto_scroll"_key] = follow_enabled_;
  }
}

void tail::reapply_filter_visibility() {
  auto reapply = [&](log_table_state& state) {
    if (!state.table_ptr)
      return;
    auto* children_p = state.table_ptr->findField<dynamic_ptr>("children"_key);
    if (!children_p || !*children_p)
      return;
    auto& children = *children_p;
    for (auto& entry : state.rows) {
      auto& row_f = (*children)[entry.child_key];
      if (row_f.is<dynamic_ptr>()) {
        if (auto row = row_f.as<dynamic_ptr>())
          row["visible"_key] = passes_filter(entry.raw);
      }
    }
  };

  reapply(all_table_);
  for (auto& [tag, state] : tag_tabs_)
    reapply(state.table);
}

void tail::apply_filter_from_input() {
  if (!filter_input_ptr_)
    return;
  dynamic args;
  args["pattern"_key] = filter_input_ptr_->as<std::string>("value"_key);
  do_set_filter(args);
}

void tail::clear_all() {
  auto clear_table = [&](log_table_state& state) {
    if (!state.table_ptr)
      return;
    auto* children_p = state.table_ptr->findField<dynamic_ptr>("children"_key);
    if (children_p && *children_p) {
      for (auto& row : state.rows) {
        (*children_p)->erase(row.child_key);
        ctx().objects.erase(row.row_id.id);
        ctx().objects.erase(row.time_id.id);
        ctx().objects.erase(row.level_id.id);
        ctx().objects.erase(row.tag_id.id);
        ctx().objects.erase(row.source_id.id);
        ctx().objects.erase(row.message_id.id);
      }
      state.table_ptr->refresh_children_order();
    }
    state.rows.clear();
  };

  clear_table(all_table_);
  for (auto& [tag, state] : tag_tabs_)
    clear_table(state.table);

  total_lines_received_ = 0;
  update_status();
}

void tail::update_status() {
  if (!status_label_ptr_)
    return;
  std::ostringstream oss;
  oss << total_lines_received_ << (total_lines_received_ == 1 ? " line" : " lines");
  if (!filter_pattern_.empty())
    oss << "  --  filter: /" << filter_pattern_ << "/";
  status_label_ptr_["text"_key] = oss.str();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void tail::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == filter_input_id_ && event == "changed"_key) {
    apply_filter_from_input();
    return;
  }

  if (id == lines_input_id_ && event == "changed"_key) {
    dynamic args;
    if (auto* v = payload.findField<int32_t>("value"_key))
      args["count"_key] = *v;
    int32_t count = do_set_line_count(args).as<int32_t>("count"_key);

    // do_set_line_count()'s own evict_to_cap() only ever trims -- it has no
    // way to pull in *more* history when the user raises the count, since
    // this form never touches the filesystem. Only the interactive path
    // asks the client to do that: clear every table (so the client's
    // rescanned lines replace what's shown instead of appending after it,
    // which would duplicate whatever's still within the new count) and
    // emit 'rescan_requested' for the client to react to.
    clear_all();
    dynamic payload_out;
    payload_out["line_count"_key] = count;
    emit("rescan_requested"_key, std::move(payload_out));
    return;
  }

  if (id == follow_checkbox_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<bool>("value"_key))
      follow_enabled_ = *v;
    apply_follow_state();
    return;
  }

  if (id == btn_clear_id_ && event == "clicked"_key) {
    clear_all();
    return;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_tail() {
  auto proto = dynamic_ptr{"Tail"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Tail"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addMethod(
      "push_lines"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<tail&>(self).do_push_lines(args);
      }});

  proto->addMethod(
      "set_filter"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<tail&>(self).do_set_filter(args);
      }});

  proto->addMethod(
      "set_line_count"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<tail&>(self).do_set_line_count(args);
      }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Tail"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("`tail`-like log viewer. The client owns reading (and, with -f, "
                        "following) local log files and calls push_lines with each new batch "
                        "of raw text; this form parses/colorizes them (severity level, "
                        "[Tag] tokens -- rules configurable via the module's patterns.json "
                        "resource), renders them into a scrolling table, and mirrors any line "
                        "carrying a [Tag] into its own dedicated tab. The toolbar's Follow "
                        "checkbox controls whether the table(s) auto-scroll to the newest row "
                        "as lines arrive; unchecking it lets the user browse older rows without "
                        "being pulled back to the bottom. "
                        "set_filter applies a live regex filter to row *visibility*: every "
                        "ingested line always becomes a row, and changing the pattern "
                        "immediately shows/hides already-received rows to match, so the filter "
                        "can be edited dynamically to search through lines already on screen. "
                        "set_line_count sets the toolbar's Lines field, which doubles as a live "
                        "per-table row cap: whenever a table would hold more rows than this, the "
                        "oldest are dropped immediately, whether growth came from a newly "
                        "ingested line or from lowering the field itself. This form has no "
                        "filesystem access, so it cannot pull in more history on its own -- "
                        "editing the field in the UI additionally clears every table and emits "
                        "'rescan_requested' with {line_count: int} for the client to answer by "
                        "re-reading each tailed file's last N lines, so raising the count really "
                        "does surface more history. Listen for the 'closed' event to detect when "
                        "the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<tail>("wish"_key, "Tail"_key));
}

} // namespace bdg::wish
