// MIT License © 2026 Binary Dice Games
/// @file find.cpp
/// @brief Implementation of the Find form.
///
/// No filesystem access here: the files being searched live on the
/// client's machine, so the index and every query run in
/// modules/bdg/desktop/find/client/. See find.hpp's class doc comment for
/// the client/server handshake.
#include "find.hpp"

#include "modules/bdg/desktop/find/find_settings.hpp"

#include "src/bison/bison_object.hpp"
#include "ui/forms/file_browser_utils.hpp"

#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cstdio>

namespace bdg::wish {

using namespace bison;

namespace {

std::string format_count(std::int64_t n) {
  std::string digits = std::to_string(n < 0 ? 0 : n);
  for (ptrdiff_t i = static_cast<ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3)
    digits.insert(static_cast<size_t>(i), 1, ',');
  return digits;
}

/// Accent color of the active search-mode indicators.
constexpr common::theme_color kAccent{"#0969DAFF", "#58A6FFFF"};

/// The filter drop-down's entries, in find_db::filter_kind order.
constexpr const char* kFilterKeys[] = {
    "$$FIND_FILTER_ALL",       "$$FIND_FILTER_FOLDERS",   "$$FIND_FILTER_FILES",
    "$$FIND_FILTER_APPLICATIONS", "$$FIND_FILTER_ARCHIVES", "$$FIND_FILTER_AUDIO",
    "$$FIND_FILTER_DOCUMENTS", "$$FIND_FILTER_PICTURES",  "$$FIND_FILTER_VIDEOS",
};
constexpr std::int32_t kFilterCount = static_cast<std::int32_t>(std::size(kFilterKeys));

} // namespace

// ── UI layout ────────────────────────────────────────────────────────────────
//
// FSearch's window: menu bar, then the search field with the filter
// drop-down on its right, the results list, and the status bar.
//
// "query" has no EnterReturnsTrue: every keystroke searches, as in FSearch.
// It stretches ("width": -1) and the filter keeps a fixed width. The table
// carries "height": -1 so it takes whatever the bars above and below leave
// (du's technique); "Hideable|Reorderable" give it ImGui's header context
// menu, FSearch's column chooser. The status bar's mode indicators start
// hidden and are shown while their mode is on.
static constexpr const char* kLayout = R"json({
  "type": "Window",
  "title": "$$FIND_TITLE",
  "width": 1000, "height": 640,
  "closable": true,
  "children": {
    "menu": {
      "type": "MenuBar",
      "children": {
        "file": {
          "type": "Menu", "label": "$$FIND_FILE",
          "children": {
            "open":        { "type": "MenuItem", "label": "$$FIND_OPEN", "shortcut": "Enter", "icon": "res/icons/open_in_new.png" },
            "open_folder": { "type": "MenuItem", "label": "$$FIND_OPEN_FOLDER", "shortcut": "Ctrl+Enter", "icon": "res/icons/folder_open.png" },
            "sep1":        { "type": "Separator" },
            "update":      { "type": "MenuItem", "label": "$$FIND_UPDATE_DATABASE", "shortcut": "Ctrl+Shift+R", "icon": "res/icons/refresh.png" },
            "sep2":        { "type": "Separator" },
            "close":       { "type": "MenuItem", "label": "$$FIND_CLOSE", "shortcut": "Ctrl+W", "icon": "res/icons/close.png" }
          }
        },
        "edit": {
          "type": "Menu", "label": "$$FIND_EDIT",
          "children": {
            "copy_name":      { "type": "MenuItem", "label": "$$FIND_COPY_NAME", "icon": "res/icons/copy.png", "enabled": false },
            "copy_path":      { "type": "MenuItem", "label": "$$FIND_COPY_PATH", "icon": "res/icons/copy.png", "enabled": false },
            "copy_full_path": { "type": "MenuItem", "label": "$$FIND_COPY_FULL_PATH", "shortcut": "Ctrl+Shift+C", "icon": "res/icons/copy.png", "enabled": false },
            "sep1":           { "type": "Separator" },
            "deselect":       { "type": "MenuItem", "label": "$$FIND_DESELECT_ALL", "shortcut": "Ctrl+Shift+A" }
          }
        },
        "view": {
          "type": "Menu", "label": "$$FIND_VIEW",
          "children": {
            "statusbar": { "type": "MenuItem", "label": "$$FIND_STATUS_BAR" },
            "filter":    { "type": "MenuItem", "label": "$$FIND_FILTER" }
          }
        },
        "search": {
          "type": "Menu", "label": "$$FIND_SEARCH",
          "children": {
            "match_case":     { "type": "MenuItem", "label": "$$FIND_MATCH_CASE", "shortcut": "Ctrl+I" },
            "search_in_path": { "type": "MenuItem", "label": "$$FIND_SEARCH_IN_PATH", "shortcut": "Ctrl+U" },
            "regex":          { "type": "MenuItem", "label": "$$FIND_REGULAR_EXPRESSION", "shortcut": "Ctrl+R" }
          }
        },
        "help": {
          "type": "Menu", "label": "$$FIND_HELP",
          "children": {
            "syntax": { "type": "MenuItem", "label": "$$FIND_SEARCH_SYNTAX", "icon": "res/icons/help.png" },
            "about":  { "type": "MenuItem", "label": "$$FIND_ABOUT", "icon": "res/icons/info.png" }
          }
        }
      }
    },
    "main": {
      "type": "VerticalLayout",
      "children": {
        "bar": {
          "type": "HorizontalLayout",
          "spacing": 6,
          "children": {
            "query":  { "type": "InputText", "hint": "$$FIND_SEARCH_HINT", "value": "", "max_length": 4096, "width": -1 },
            "filter": { "type": "Combo", "width": 150 }
          }
        },
        "table": {
          "type": "Table", "columns": 5, "headers": true,
          "flags": "Resizable|Reorderable|Hideable|Sortable|RowBg|BordersInnerV|ScrollY", "outer_width": 0, "height": -1,
          "auto_scroll": false, "cell_tooltips": true,
          "children": {
            "col_name":     { "type": "TableColumn", "label": "$$FIND_NAME", "flags": "WidthStretch|DefaultSort|NoHide", "init_width": 1.2, "column_id": 0 },
            "col_path":     { "type": "TableColumn", "label": "$$FIND_PATH", "flags": "WidthStretch", "init_width": 2.0, "column_id": 1 },
            "col_type":     { "type": "TableColumn", "label": "$$FIND_TYPE", "flags": "WidthFixed", "init_width": 140, "column_id": 2 },
            "col_size":     { "type": "TableColumn", "label": "$$FIND_SIZE", "flags": "WidthFixed|PreferSortDescending", "init_width": 80, "column_id": 3 },
            "col_modified": { "type": "TableColumn", "label": "$$FIND_DATE_MODIFIED", "flags": "WidthFixed|PreferSortDescending", "init_width": 150, "column_id": 4 }
          }
        },
        "statusbar": {
          "type": "HorizontalLayout",
          "spacing": 12,
          "children": {
            "status":    { "type": "Label", "text": "$$FIND_LOADING_DATABASE" },
            "selection": { "type": "Label", "text": "", "visible": false },
            "spring":    { "type": "Spring" },
            "case":      { "type": "Label", "text": "$$FIND_INDICATOR_CASE", "tooltip": "$$FIND_MATCH_CASE", "visible": false },
            "path":      { "type": "Label", "text": "$$FIND_INDICATOR_PATH", "tooltip": "$$FIND_SEARCH_IN_PATH", "visible": false },
            "regex":     { "type": "Label", "text": "$$FIND_INDICATOR_REGEX", "tooltip": "$$FIND_REGULAR_EXPRESSION", "visible": false },
            "filter":    { "type": "Label", "text": "", "visible": false },
            "db":        { "type": "Label", "text": "$$FIND_DATABASE_EMPTY", "tooltip": "$$FIND_DATABASE_TOOLTIP" }
          }
        }
      }
    }
  }
})json";

// ── find_form ────────────────────────────────────────────────────────────────

std::string find_form::result_row::full_path() const {
  if (path.empty())
    return name;
  const char last = path.back();
  return last == '/' || last == '\\' ? path + name : path + "/" + name;
}

find_form::find_form(dynamic&& base) : tool_form(std::move(base)) {
  i18n_prefix_ = "bdg/desktop/find";
}

find_form::~find_form() = default;

void find_form::on_init() {
  internal_root_key_ = next_available_key("__find_");

  match_case_ = setting_bool(find_settings::match_case());
  search_in_path_ = setting_bool(find_settings::search_in_path());
  regex_ = setting_bool(find_settings::regex());
  show_statusbar_ = setting_bool(find_settings::show_statusbar());
  show_filter_ = setting_bool(find_settings::show_filter());

  build_window(internal_root_key_, kLayout, window_id_, [&](ui_tree& tree) {
    if (auto* title_f = findField<std::string>("title"_key))
      tree[""]["title"_key] = tr(*title_f);

    tree.with("main.bar.query", [&](const auto& e) {
      query_ptr_ = e;
      query_id_ = wish_id_of(e);
    });
    tree.with("main.bar.filter", [&](const auto& e) {
      filter_ptr_ = e;
      filter_id_ = wish_id_of(e);
      std::string items;
      for (const char* key : kFilterKeys)
        items += (items.empty() ? "" : "\n") + tr(key);
      e["items"_key] = items;
      e["value"_key] = int32_t{0};
    });
    tree.with("main.table", [&](const auto& e) {
      table_ptr_ = e;
      table_id_ = wish_id_of(e);
    });
    tree.with("main.statusbar", [&](const auto& e) { statusbar_ptr_ = e; });
    tree.with("main.statusbar.status", [&](const auto& e) { status_label_ptr_ = e; });
    tree.with("main.statusbar.selection", [&](const auto& e) { selection_label_ptr_ = e; });
    tree.with("main.statusbar.case", [&](const auto& e) {
      case_label_ptr_ = e;
      common::set_text_color(e, kAccent);
    });
    tree.with("main.statusbar.path", [&](const auto& e) {
      path_label_ptr_ = e;
      common::set_text_color(e, kAccent);
    });
    tree.with("main.statusbar.regex", [&](const auto& e) {
      regex_label_ptr_ = e;
      common::set_text_color(e, kAccent);
    });
    tree.with("main.statusbar.filter", [&](const auto& e) {
      filter_label_ptr_ = e;
      common::set_text_color(e, kAccent);
    });
    tree.with("main.statusbar.db", [&](const auto& e) { db_label_ptr_ = e; });

    auto item = [&](const char* path, ui_element_ptr* out, click_handler handler) {
      tree.with(path, [&](const auto& e) {
        if (out)
          *out = e;
        on_click(wish_id_of(e), std::move(handler));
      });
    };
    item("menu.file.open", nullptr, [this] { open_rows(selected_rows(), false); });
    item("menu.file.open_folder", nullptr, [this] { open_rows(selected_rows(), true); });
    item("menu.file.update", nullptr, [this] { emit("on_update_database"_key); });
    item("menu.file.close", nullptr, [this] {
      emit("closed"_key);
      remove_internal_objects();
    });
    // The copy items copy on the render thread (MenuItem.copy_text); their
    // clicks need no handling.
    item("menu.edit.copy_name", &menu_copy_name_ptr_, [] {});
    item("menu.edit.copy_path", &menu_copy_path_ptr_, [] {});
    item("menu.edit.copy_full_path", &menu_copy_full_path_ptr_, [] {});
    item("menu.edit.deselect", nullptr, [this] {
      selected_.clear();
      anchor_ = -1;
      apply_selection();
    });
    item("menu.view.statusbar", &menu_statusbar_ptr_, [this] {
      show_statusbar_ = !show_statusbar_;
      save_setting(find_settings::show_statusbar(), show_statusbar_);
      update_view();
    });
    item("menu.view.filter", &menu_filter_ptr_, [this] {
      show_filter_ = !show_filter_;
      save_setting(find_settings::show_filter(), show_filter_);
      update_view();
    });
    item("menu.search.match_case", &menu_match_case_ptr_, [this] {
      toggle_mode(match_case_, find_settings::match_case());
    });
    item("menu.search.search_in_path", &menu_search_in_path_ptr_, [this] {
      toggle_mode(search_in_path_, find_settings::search_in_path());
    });
    item("menu.search.regex", &menu_regex_ptr_, [this] { toggle_mode(regex_, find_settings::regex()); });
    item("menu.help.syntax", nullptr, [this] { show_syntax_help(); });
    item("menu.help.about", nullptr, [this] {
      show_message("$$FIND_ABOUT", "$$FIND_ABOUT_TEXT", "info");
    });
  });

  update_indicators();
  update_view();
  update_status();
}

// ── Search requests ──────────────────────────────────────────────────────────

void find_form::request_search() {
  dynamic req;
  req["query"_key] = query_;
  req["filter"_key] = filter_;
  req["match_case"_key] = match_case_;
  req["search_in_path"_key] = search_in_path_;
  req["regex"_key] = regex_;
  req["sort_column"_key] = sort_column_;
  req["ascending"_key] = ascending_;
  req["generation"_key] = ++generation_;
  emit("on_search"_key, std::move(req));
}

void find_form::toggle_mode(bool& mode, const settings::setting_info& info) {
  mode = !mode;
  save_setting(info, mode);
  update_indicators();
  request_search();
}

// ── Table ────────────────────────────────────────────────────────────────────

void find_form::fill_table() {
  if (!table_ptr_)
    return;
  auto* children_p = table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  // Drops the previous rows (indexed); the named TableColumn children remain.
  children->clear();
  erase_objects(row_object_ids_);
  row_object_ids_.clear();
  row_menu_.clear();

  auto label = [](const std::string& text, int32_t order) {
    ui_element_ptr lbl = ui_element_ptr::create("wish"_key, "Label"_key);
    lbl["text"_key] = text;
    lbl["order"_key] = order;
    return lbl;
  };
  auto registered = [&](const char* cls) {
    ui_element_ptr el = ui_element_ptr::create("wish"_key, key_t{cls});
    assign_id(el);
    row_object_ids_.push_back(wish_id_of(el));
    return el;
  };

  for (size_t i = 0; i < rows_.size(); ++i) {
    const result_row& r = rows_[i];
    const auto idx = static_cast<int32_t>(i);

    ui_element_ptr menu = registered("ContextMenu");
    auto menu_item = [&](const char* text, const char* icon, std::optional<row_action> action,
                         const std::string& copy_text = {}) {
      ui_element_ptr it = registered(action ? "MenuItem" : "Separator");
      if (action) {
        it["label"_key] = tr(text);
        common::set_icon(it, icon);
        if (!copy_text.empty())
          it["copy_text"_key] = copy_text;
        row_menu_[wish_id_of(it)] = {*action, idx};
      }
      return it;
    };
    common::set_children_list(
        menu, {menu_item("$$FIND_OPEN", "open_in_new", row_action::open),
               menu_item("$$FIND_OPEN_FOLDER", "folder_open", row_action::open_folder), menu_item("", "", {}),
               menu_item("$$FIND_COPY_NAME", "copy", row_action::copy, r.name),
               menu_item("$$FIND_COPY_PATH", "copy", row_action::copy, r.path),
               menu_item("$$FIND_COPY_FULL_PATH", "copy", row_action::copy, r.full_path())});

    ui_element_ptr name_cell = make_name_cell(r.name, r.dir ? "dir" : "file", r.name);
    name_cell["order"_key] = int32_t{0};

    ui_element_ptr row_el = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row_el["order"_key] = idx;
    row_el["selected"_key] = selected_.count(idx) > 0;
    auto cells = dynamic_ptr{key_t{0U}, {}};
    (*cells)[size_t{0}] = dynamic_ptr{name_cell};
    (*cells)[size_t{1}] = dynamic_ptr{label(r.path, 1)};
    (*cells)[size_t{2}] = dynamic_ptr{label(tr(r.type), 2)};
    (*cells)[size_t{3}] = dynamic_ptr{label(r.size, 3)};
    (*cells)[size_t{4}] = dynamic_ptr{label(r.modified, 4)};
    menu["order"_key] = int32_t{5};
    (*cells)[size_t{5}] = dynamic_ptr{menu};
    row_el["children"_key] = cells;
    row_el->refresh_children_order();
    (*children)[i] = dynamic_ptr{row_el};
  }
  table_ptr_->refresh_children_order();
}

std::vector<const find_form::result_row*> find_form::selected_rows() const {
  std::vector<const result_row*> out;
  for (int32_t i : selected_)
    if (i >= 0 && static_cast<size_t>(i) < rows_.size())
      out.push_back(&rows_[static_cast<size_t>(i)]);
  return out;
}

void find_form::apply_selection() {
  if (table_ptr_) {
    if (auto* cf = table_ptr_->findField<dynamic_ptr>("children"_key); cf && *cf) {
      (*cf)->forEach([&](key_t, const field& f) {
        auto* rp = f.get<dynamic_ptr>();
        if (!rp || !*rp || (*rp)->as<key_t>(dynamic::CLASS) != "TableRow"_key)
          return;
        auto& row = **rp;
        row["selected"_key] = selected_.count(row.as<int32_t>("order"_key)) > 0;
      });
    }
  }

  std::string names, paths, full_paths;
  for (const result_row* r : selected_rows()) {
    const char* sep = names.empty() ? "" : "\n";
    names += sep + r->name;
    paths += sep + r->path;
    full_paths += sep + r->full_path();
  }
  const bool any = !selected_.empty();
  for (auto [ptr, text] : {std::pair{&menu_copy_name_ptr_, &names}, std::pair{&menu_copy_path_ptr_, &paths},
                           std::pair{&menu_copy_full_path_ptr_, &full_paths}}) {
    if (!*ptr)
      continue;
    (*ptr)["copy_text"_key] = *text;
    (*ptr)["enabled"_key] = any;
  }

  if (selection_label_ptr_) {
    selection_label_ptr_["visible"_key] = any;
    selection_label_ptr_["text"_key] =
        trf("$$FIND_N_OF_M_SELECTED", {format_count(static_cast<int64_t>(selected_.size())), format_count(total_)});
  }
}

void find_form::open_rows(const std::vector<const result_row*>& rows, bool folder) {
  if (rows.empty())
    return;
  std::string paths;
  for (const result_row* r : rows)
    paths += (paths.empty() ? "" : "\n") + r->full_path();
  dynamic req;
  req["paths"_key] = paths;
  emit(folder ? "on_open_folder"_key : "on_open"_key, std::move(req));
}

// ── Status bar and menus ─────────────────────────────────────────────────────

void find_form::update_status() {
  if (!status_label_ptr_)
    return;
  std::string text;
  if (!error_.empty()) {
    text = trf("$$FIND_INVALID_QUERY", {error_});
    common::set_text_color(status_label_ptr_, common::kBad);
  } else {
    common::set_text_color(status_label_ptr_, common::kIdle);
    if (db_files_ < 0 && !has_results_)
      text = index_status_.empty() ? tr("$$FIND_LOADING_DATABASE") : tr(index_status_);
    else if (!has_results_)
      text = tr("$$FIND_QUERYING");
    else if (static_cast<int32_t>(rows_.size()) < total_)
      text = trf(
          "$$FIND_SHOWING_N_OF_M_ITEMS",
          {format_count(static_cast<int64_t>(rows_.size())), format_count(total_)});
    else
      text = trf(total_ == 1 ? "$$FIND_N_ITEM" : "$$FIND_N_ITEMS", {format_count(total_)});
    // A re-index runs with the previous database still searchable.
    if (indexing_ && db_files_ >= 0)
      text += "  -  " + (index_status_.empty() ? tr("$$FIND_UPDATING_DATABASE") : tr(index_status_));
  }
  status_label_ptr_["text"_key] = text;
}

void find_form::update_indicators() {
  if (case_label_ptr_)
    case_label_ptr_["visible"_key] = match_case_;
  if (path_label_ptr_)
    path_label_ptr_["visible"_key] = search_in_path_;
  if (regex_label_ptr_)
    regex_label_ptr_["visible"_key] = regex_;
  if (filter_label_ptr_) {
    filter_label_ptr_["visible"_key] = filter_ != 0;
    filter_label_ptr_["text"_key] = tr(kFilterKeys[std::clamp(filter_, 0, kFilterCount - 1)]);
  }
  if (menu_match_case_ptr_)
    menu_match_case_ptr_["checked"_key] = match_case_;
  if (menu_search_in_path_ptr_)
    menu_search_in_path_ptr_["checked"_key] = search_in_path_;
  if (menu_regex_ptr_)
    menu_regex_ptr_["checked"_key] = regex_;
}

void find_form::update_view() {
  if (statusbar_ptr_)
    statusbar_ptr_["visible"_key] = show_statusbar_;
  if (filter_ptr_)
    filter_ptr_["visible"_key] = show_filter_;
  if (menu_statusbar_ptr_)
    menu_statusbar_ptr_["checked"_key] = show_statusbar_;
  if (menu_filter_ptr_)
    menu_filter_ptr_["checked"_key] = show_filter_;
}

void find_form::show_syntax_help() {
  show_message("$$FIND_SEARCH_SYNTAX", "$$FIND_SYNTAX_TEXT", "info");
}

// ── RMI methods ──────────────────────────────────────────────────────────────

dynamic find_form::do_show_results(const dynamic& args) {
  if (auto* g = args.findField<int32_t>("generation"_key); g && *g != generation_)
    return dynamic{}; // a newer search is on its way

  rows_.clear();
  common::for_each_entry(args, "entries"_key, [&](const dynamic& e) {
    result_row r;
    r.name = common::str_of(e, "name"_key);
    r.path = common::str_of(e, "path"_key);
    r.type = common::str_of(e, "type"_key);
    r.size = common::str_of(e, "size"_key);
    r.modified = common::str_of(e, "modified"_key);
    r.dir = common::flag_of(e, "dir"_key);
    rows_.push_back(std::move(r));
  });
  total_ = 0;
  total_folders_ = 0;
  if (auto* v = args.findField<int32_t>("total"_key))
    total_ = *v;
  if (auto* v = args.findField<int32_t>("folders"_key))
    total_folders_ = *v;
  error_ = common::str_of(args, "error"_key);
  has_results_ = true;

  // A new result list: FSearch keeps no selection across queries.
  selected_.clear();
  anchor_ = -1;
  fill_table();
  apply_selection();
  update_status();
  return dynamic{};
}

dynamic find_form::do_search_again() {
  request_search();
  return dynamic{};
}

dynamic find_form::on_set(const dynamic& patch) {
  if (auto* v = patch.findField<bool>("indexing"_key))
    indexing_ = *v;
  if (auto* v = patch.findField<std::string>("status"_key))
    index_status_ = *v;
  if (auto* v = patch.findField<int32_t>("db_files"_key))
    db_files_ = *v;
  if (auto* v = patch.findField<int32_t>("db_folders"_key))
    db_folders_ = *v;
  if (db_label_ptr_ && db_files_ >= 0)
    db_label_ptr_["text"_key] = trf(
        "$$FIND_DATABASE_COUNTS",
        {format_count(db_files_), format_count(db_folders_)});
  update_status();
  return patch;
}

// ── Event routing ────────────────────────────────────────────────────────────

void find_form::on_event(key_t id, key_t event, const dynamic& payload) {
  if (event == "closed"_key && id == window_id_) {
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == query_id_ && event == "changed"_key) {
    query_ = common::str_of(payload, "value"_key);
    request_search();
    return;
  }

  if (id == filter_id_ && event == "changed"_key) {
    if (auto* v = payload.findField<int32_t>("value"_key))
      filter_ = std::clamp(*v, 0, kFilterCount - 1);
    update_indicators();
    request_search();
    return;
  }

  if (id == table_id_) {
    if (event == "sorted"_key) {
      auto* col_f = payload.findField<int32_t>("column_id"_key);
      auto* asc_f = payload.findField<bool>("ascending"_key);
      if (!col_f || !asc_f)
        return;
      sort_column_ = *col_f;
      ascending_ = *asc_f;
      request_search();
      return;
    }
    if (event == "row_selected"_key || event == "row_activated"_key) {
      const int32_t idx = payload.as<int32_t>("index"_key);
      if (idx < 0 || static_cast<size_t>(idx) >= rows_.size())
        return;
      if (event == "row_activated"_key) {
        selected_ = {idx};
        anchor_ = idx;
        apply_selection();
        open_rows({&rows_[static_cast<size_t>(idx)]}, false);
        return;
      }
      if (common::flag_of(payload, "shift"_key) && anchor_ >= 0) {
        if (!common::flag_of(payload, "ctrl"_key))
          selected_.clear();
        for (int32_t i = std::min(anchor_, idx); i <= std::max(anchor_, idx); ++i)
          selected_.insert(i);
      } else if (common::flag_of(payload, "ctrl"_key)) {
        if (!selected_.erase(idx))
          selected_.insert(idx);
        anchor_ = idx;
      } else {
        selected_ = {idx};
        anchor_ = idx;
      }
      apply_selection();
    }
    return;
  }

  if (event == "clicked"_key) {
    if (auto it = row_menu_.find(id); it != row_menu_.end()) {
      const auto [action, idx] = it->second;
      if (idx < 0 || static_cast<size_t>(idx) >= rows_.size())
        return;
      if (action == row_action::open || action == row_action::open_folder)
        open_rows({&rows_[static_cast<size_t>(idx)]}, action == row_action::open_folder);
      return;
    }
    dispatch_click(id);
  }
}

// ── Registration ─────────────────────────────────────────────────────────────

void register_find() {
  auto proto = dynamic_ptr{"Find"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"$$FIND_TITLE"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title; a \"$$KEY\" is translated (see docs/i18n.md)."),
          attr<Category>("Appearance")});

  proto->addField(
      "status"_key,
      field{
          std::string{""},
          attr<DisplayName>("Status"),
          attr<Description>("Progress of the database update, shown in the status bar while `indexing`; a "
                            "\"$$KEY\" or make_message() is translated (see docs/i18n.md)."),
          attr<Category>("Data")});

  proto->addField(
      "indexing"_key,
      field{
          bool{false},
          attr<DisplayName>("Indexing"),
          attr<Description>("True while the client builds or updates its database."),
          attr<Category>("State")});

  proto->addField(
      "db_files"_key,
      field{
          int32_t{-1},
          attr<DisplayName>("Database files"),
          attr<Description>("Files in the client's database, shown in the status bar; -1 until the first "
                            "database is ready."),
          attr<Category>("Data")});

  proto->addField(
      "db_folders"_key,
      field{
          int32_t{0},
          attr<DisplayName>("Database folders"),
          attr<Description>("Folders in the client's database, shown in the status bar."),
          attr<Category>("Data")});

  proto->addMethod(
      "show_results"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
        return static_cast<find_form&>(self).do_show_results(args);
      }});
  proto->addMethod("search_again"_key, bison::method{[](dynamic& self, const dynamic&) -> dynamic {
                     return static_cast<find_form&>(self).do_search_again();
                   }});
  proto->addMethod("__setter"_key, bison::method{[](dynamic& s, const dynamic& p) -> dynamic {
                     return static_cast<find_form&>(s).on_set(p);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Find"));
  (*proto)[dynamic::CLASS].addAttribute(attr<Description>(
      "Instant file search over a database of the client's folders, in the spirit of FSearch. The "
      "server owns the UI only; listen for on_search/on_update_database/on_open/on_open_folder to "
      "drive the client half (answering on_search with show_results()), and 'closed' to detect when "
      "the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<find_form>("wish"_key, "Find"_key));

  common::register_tool_settings(find_settings::all());
}

} // namespace bdg::wish
