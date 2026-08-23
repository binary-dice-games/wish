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

/// Every table (the "All" tab's, and each dynamically-created tag tab's)
/// keeps at most this many rows, oldest evicted first -- a live tail could
/// otherwise run for hours and accumulate an unbounded RMI object count.
constexpr size_t kMaxBufferedRows = 2000;

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────
//
// ImGuiInputTextFlags_EnterReturnsTrue = 32: the filter box only fires
// "changed" on Enter, not on every keystroke -- filtering re-walks nothing
// retroactively (see class doc comment) so this just avoids a "changed"
// storm while the user is still typing a pattern.
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
            "filter_input": { "type": "InputText", "label": "Filter (regex)", "hint": "e.g. error|timeout", "width": 320, "flags": "EnterReturnsTrue" },
            "btn_apply_filter": { "type": "Button", "label": "Apply", "width": 80 },
            "btn_clear_filter": { "type": "Button", "label": "Clear Filter", "width": 100 },
            "btn_clear": { "type": "Button", "label": "Clear All", "width": 90 }
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
                  "outer_height": -1, "flags": "Resizable|RowBg|Borders|ScrollY",
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
  tree.with("vbox.toolbar.btn_apply_filter", [&](const auto& e) { btn_apply_filter_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_clear_filter", [&](const auto& e) { btn_clear_filter_id_ = wish_id_of(e); });
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
  update_status();
  return dynamic{};
}

// ── Line ingestion / rendering ────────────────────────────────────────────────

void tail::ingest_line(const std::string& raw, const std::string& source) {
  parsed_log_line pl = parser_.parse(raw, source);
  ++total_lines_received_;

  if (passes_filter(raw)) {
    append_row(all_table_, pl);
    if (!pl.tag.empty())
      append_row(ensure_tag_tab(pl.tag).table, pl);
  }

  update_status();
}

bool tail::passes_filter(const std::string& raw) const {
  if (!filter_regex_)
    return true;
  return std::regex_search(raw, *filter_regex_);
}

void tail::append_row(log_table_state& state, const parsed_log_line& pl) {
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
    ui_element_ptr cell{dynamic::instantiate("wish"_key, "Label"_key)};
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

  ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
  row["order"_key] = static_cast<int32_t>(state.next_child_key);

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
      wish_id_of(source_cell), wish_id_of(message_cell)});

  // FIFO cap: a live tail has no natural upper bound on how many lines it
  // will ever see, so the oldest row (and every RMI object it owns) is
  // dropped once the cap is exceeded -- mirrors editor.cpp's append_log_row.
  if (state.rows.size() > kMaxBufferedRows) {
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

  state.table_ptr->refresh_children_order();
}

// ── Tag tabs ───────────────────────────────────────────────────────────────────

ui_element_ptr tail::build_log_table(log_table_state& state) {
  ui_element_ptr table{dynamic::instantiate("wish"_key, "Table"_key)};
  table["id"_key] = "##tail_tbl_" + std::to_string(next_table_seq_++);
  table["columns"_key] = int32_t{5};
  table["headers"_key] = true;
  table["outer_height"_key] = -1.0f;
  table["flags"_key] = int32_t{33556417}; // Resizable|RowBg|Borders|ScrollY -- see kLayout's comment.

  auto make_col = [&](const char* label, int32_t col_id, int32_t flags, float w, int32_t order) {
    ui_element_ptr col{dynamic::instantiate("wish"_key, "TableColumn"_key)};
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

  auto children = dynamic_ptr{key_t{0U}, {}};
  (*children)[size_t{0}] = dynamic_ptr{make_col("Time", 0, 16, 90.0f, 0)};
  (*children)[size_t{1}] = dynamic_ptr{make_col("Level", 1, 16, 70.0f, 1)};
  (*children)[size_t{2}] = dynamic_ptr{make_col("Tag", 2, 16, 100.0f, 2)};
  (*children)[size_t{3}] = dynamic_ptr{make_col("Source", 3, 16, 120.0f, 3)};
  (*children)[size_t{4}] = dynamic_ptr{make_col("Message", 4, 8, 0.0f, 4)};
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

  ui_element_ptr tab{dynamic::instantiate("wish"_key, "TabItem"_key)};
  tab["label"_key] = "[" + tag + "]";
  tab["closable"_key] = true;
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
      state.tab_bar_child_key = child_key;
      tab_bar_ptr_->refresh_children_order();
    }
  }

  state.tab_ptr = tab;
  state.tab_id = tab_id;
  tab_id_to_tag_[tab_id.id] = tag;

  auto [ins_it, ok] = tag_tabs_.emplace(tag, std::move(state));
  return ins_it->second;
}

void tail::close_tag_tab(const std::string& tag) {
  auto it = tag_tabs_.find(tag);
  if (it == tag_tabs_.end())
    return;
  auto& state = it->second;

  if (tab_bar_ptr_) {
    if (auto* children_p = tab_bar_ptr_->findField<dynamic_ptr>("children"_key); children_p && *children_p)
      (*children_p)->erase(state.tab_bar_child_key);
    tab_bar_ptr_->refresh_children_order();
  }

  // Sweep every RMI object this tag's tab owns (its rows' cells, its
  // TableColumns, the Table itself) -- children map entries alone don't
  // release ctx().objects (see append_row()'s FIFO-eviction comment).
  if (state.table.table_ptr) {
    if (auto* cols_p = state.table.table_ptr->findField<dynamic_ptr>("children"_key); cols_p && *cols_p) {
      (*cols_p)->forEach([&](key_t, const field& f) {
        if (!f.is<dynamic_ptr>())
          return;
        if (auto ptr = f.as<dynamic_ptr>())
          ctx().objects.erase(ptr->as<key_t>("__wish_id"_key).id);
      });
    }
    ctx().objects.erase(state.table.table_ptr->as<key_t>("__wish_id"_key).id);
  }
  ctx().objects.erase(state.tab_id.id);

  tab_id_to_tag_.erase(state.tab_id.id);
  tag_tabs_.erase(it);
}

// ── Toolbar actions ───────────────────────────────────────────────────────────

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

void tail::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == window_id_ && event == "closed"_key) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if ((id == filter_input_id_ && event == "changed"_key) || (id == btn_apply_filter_id_ && event == "clicked"_key)) {
    apply_filter_from_input();
    return;
  }

  if (id == btn_clear_filter_id_ && event == "clicked"_key) {
    if (filter_input_ptr_)
      filter_input_ptr_["value"_key] = std::string{};
    dynamic args;
    args["pattern"_key] = std::string{};
    do_set_filter(args);
    return;
  }

  if (id == btn_clear_id_ && event == "clicked"_key) {
    clear_all();
    return;
  }

  if (auto it = tab_id_to_tag_.find(id.id); it != tab_id_to_tag_.end() && event == "closed"_key) {
    close_tag_tab(it->second);
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

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Tail"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("`tail`-like log viewer. The client owns reading (and, with -f, "
                        "following) local log files and calls push_lines with each new batch "
                        "of raw text; this form parses/colorizes them (severity level, "
                        "[Tag] tokens -- rules configurable via the module's patterns.json "
                        "resource), renders them into a scrolling, auto-following table, and "
                        "mirrors any line carrying a [Tag] into its own dedicated tab. "
                        "set_filter applies a live regex filter prospectively only -- like "
                        "`tail -f | grep`, it does not retroactively hide/show already-received "
                        "rows. Listen for the 'closed' event to detect when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<tail>("wish"_key, "Tail"_key));
}

} // namespace bdg::wish
