// MIT License © 2025 Binary Dice Games
/// @file nano.cpp
/// @brief Implementation of the Nano form.
#include "nano.hpp"

#include "src/bison/bison_object.hpp"

#include <context/file_service.hpp>
#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace bdg::wish {

using namespace bison;
using common::wish_id_of;

namespace {

// Map a sandbox file's extension to one of TextEditor's supported "language"
// values (see src/ui/ui_elements/text_editor.cpp). Unknown extensions fall back
// to "none" (no highlighting).
std::string language_for_extension(const std::string& path) {
  auto ext = std::filesystem::path(path).extension().string();
  for (auto& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".hpp" || ext == ".hh" || ext == ".hxx")
    return "cpp";
  if (ext == ".c" || ext == ".h")
    return "c";
  if (ext == ".cs")
    return "cs";
  if (ext == ".glsl" || ext == ".vert" || ext == ".frag")
    return "glsl";
  if (ext == ".hlsl")
    return "hlsl";
  if (ext == ".lua")
    return "lua";
  if (ext == ".py")
    return "python";
  if (ext == ".sql")
    return "sql";
  if (ext == ".json")
    return "json";
  if (ext == ".md" || ext == ".markdown")
    return "markdown";
  if (ext == ".as")
    return "angelscript";
  return "none";
}

// Every language TextEditor's own "language" field recognizes (see
// text_editor.cpp's field Description) -- shared by language_for_extension()
// above (the seeded default) and each file window's language Combo item
// list, so the two never drift out of sync.
constexpr const char* kLanguages[] = {
    "none", "cpp", "c", "cs", "glsl", "hlsl", "lua", "python", "sql", "json", "markdown", "angelscript"};
constexpr int32_t kLanguageCount = int32_t(sizeof(kLanguages) / sizeof(kLanguages[0]));

int32_t language_index(const std::string& lang) {
  for (int32_t i = 0; i < kLanguageCount; ++i)
    if (lang == kLanguages[i])
      return i;
  return 0; // "none"
}

// Builds the Combo's newline-separated "items" field from kLanguages.
std::string language_combo_items() {
  std::string out;
  for (int32_t i = 0; i < kLanguageCount; ++i) {
    if (i)
      out += "\n";
    out += kLanguages[i];
  }
  return out;
}

// The nested dockspace's id: shared by dock::viewport()/layout() and every
// file window's "dock_target", which docks it into this dockspace's central
// (documents) node.
constexpr const char* kDockId = "nano_dock";

// Cap on results-table rows: each row is a handful of RMI-free elements, but
// a search for "e" in a large file would otherwise build tens of thousands.
constexpr size_t kMaxResultRows = 2000;

// Longest line text shown in the results table's Text column.
constexpr size_t kMaxResultTextLength = 300;

// ── Search helpers ───────────────────────────────────────────────────────────

// Notepad++'s "Extended" mode: \n \r \t \0 \\ \xHH (and \oNNN, \dNNN, \bNNNNNNNN
// are rarer and left out) become the characters they name.
std::string unescape_extended(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    char c = s[++i];
    switch (c) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case '0': out += '\0'; break;
      case '\\': out += '\\'; break;
      case 'x': {
        std::string hex;
        while (hex.size() < 2 && i + 1 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])))
          hex += s[++i];
        if (hex.empty())
          out += "\\x";
        else
          out += static_cast<char>(std::stoi(hex, nullptr, 16));
        break;
      }
      default:
        out += '\\';
        out += c;
    }
  }
  return out;
}

bool is_word_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

char ascii_lower(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Number of UTF-8 code points in [begin, end) -- TextEditor positions count
// characters, not bytes.
int32_t utf8_length(const std::string& s, size_t begin, size_t end) {
  int32_t n = 0;
  for (size_t i = begin; i < end && i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
      ++n;
  return n;
}

// One match found in a file's text: byte offset and length.
struct raw_match {
  size_t pos;
  size_t len;
};

// Every match of @p needle (literal) in @p text.
std::vector<raw_match> find_literal(const std::string& text, const std::string& needle, bool match_case,
    bool whole_word) {
  std::vector<raw_match> out;
  if (needle.empty())
    return out;
  std::string hay = text;
  std::string pat = needle;
  if (!match_case) {
    std::transform(hay.begin(), hay.end(), hay.begin(), ascii_lower);
    std::transform(pat.begin(), pat.end(), pat.begin(), ascii_lower);
  }
  // Non-overlapping, like Notepad++: after a match the scan resumes past
  // its end ("x x" occurs once in "x x x").
  size_t pos = hay.find(pat);
  while (pos != std::string::npos) {
    if (whole_word) {
      bool left_ok = pos == 0 || !is_word_char(hay[pos - 1]);
      size_t end = pos + pat.size();
      bool right_ok = end >= hay.size() || !is_word_char(hay[end]);
      if (!left_ok || !right_ok) {
        pos = hay.find(pat, pos + 1);
        continue;
      }
    }
    out.push_back({pos, pat.size()});
    pos = hay.find(pat, pos + pat.size());
  }
  return out;
}

// Every non-empty match of @p re in @p text.
std::vector<raw_match> find_regex(const std::string& text, const std::regex& re) {
  std::vector<raw_match> out;
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
    if (it->length(0) == 0)
      continue; // an empty match (e.g. "x*") marks no text
    out.push_back({static_cast<size_t>(it->position(0)), static_cast<size_t>(it->length(0))});
  }
  return out;
}

std::string read_file(const std::string& full_path) {
  std::ifstream in(full_path, std::ios::binary);
  return in ? std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}} : std::string{};
}

} // namespace

// ── UI layouts ────────────────────────────────────────────────────────────────
//
// The editor is split into dockable panels inside its own nested
// DockSpaceViewport ("nano_dock"), the same multi-window pattern top, pix and
// the dev modules use, seeded into a first-run arrangement by on_init()'s
// set_default_dock_layout() call: the Toolbar as a strip along the top, the
// Search panel along the bottom, and between them an empty DockArea -- the
// dockspace's central node, which ImGui keeps even while empty. Every file
// window carries "dock_target": "nano_dock", so on first use it docks into
// that central node, tabbed with the other files; from there the user can
// re-dock, split or float it. imgui.ini owns the arrangement after the first
// run. Each Window keeps a width/height: the size a panel restores to when
// dragged out of the dock.

static constexpr const char* kToolbarLayout = R"({
  "type": "Window",
  "title": "Toolbar",
  "width": 720, "height": 60,
  "closable": true,
  "children": {
    "toolbar": {
      "type": "HorizontalLayout",
      "spacing": 8.0,
      "children": {
        "btn_open": { "type": "Button", "label": "Open", "width": 90 },
        "btn_new":  { "type": "Button", "label": "New",  "width": 90 },
        "btn_save": { "type": "Button", "label": "Save", "width": 90 },
        "btn_find": { "type": "Button", "label": "Find", "width": 90 },
        "current_label": { "type": "Label", "text": "No file open" }
      }
    }
  }
})";

// One file window. "height": -1 on the TextEditor is both its own "fill the
// rest" size (render_text_editor() treats height <= 0 as fill) and its
// VerticalLayout stretch hint, so vbox gives it whatever the language combo
// leaves -- a deterministic share of the window, rather than an auto row fed
// back its own previous fill-driven height (see tail.cpp's kLogLayout
// comment for that unbounded-growth trap).
static constexpr const char* kDocumentLayout = R"({
  "type": "Window",
  "title": "",
  "width": 720, "height": 480,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "lang": { "type": "Combo", "label": "Language", "width": 150 },
        "editor": { "type": "TextEditor", "width": 0, "height": -1 }
      }
    }
  }
})";

// The Search panel, after Notepad++'s Find dialog + "Search results" panel:
// the find options on the left (Find what, the two match checkboxes, Search
// Mode), the Find All / Count / Clear buttons in a column on the right, then a one-line summary and the results table. "find_input" is
// a plain InputText (no EnterReturnsTrue): that flag only writes the typed
// text back on Enter, so clicking a button after typing would search for the
// previous text. "options" carries "width": -1 (stretch) and "buttons" a
// fixed width, so the buttons stay a tidy column at the right edge.
// results_table's "height": -1 fills the rest of the panel; ScrollY keeps the
// header and summary pinned.
static constexpr const char* kSearchLayout = R"json({
  "type": "Window",
  "title": "Search",
  "width": 720, "height": 300,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "spacing": 4.0,
      "children": {
        "top": {
          "type": "HorizontalLayout",
          "spacing": 12.0,
          "children": {
            "options": {
              "type": "VerticalLayout",
              "width": -1,
              "spacing": 2.0,
              "children": {
                "find_row": {
                  "type": "HorizontalLayout",
                  "spacing": 6.0,
                  "children": {
                    "find_label": { "type": "Label", "text": "Find what:" },
                    "find_input": { "type": "InputText", "value": "", "hint": "text or regular expression", "width": -1 }
                  }
                },
                "checks": {
                  "type": "HorizontalLayout",
                  "spacing": 16.0,
                  "children": {
                    "chk_whole_word": { "type": "Checkbox", "label": "Match whole word only", "value": false },
                    "chk_match_case": { "type": "Checkbox", "label": "Match case", "value": false }
                  }
                },
                "mode_row": {
                  "type": "HorizontalLayout",
                  "spacing": 10.0,
                  "children": {
                    "mode_label": { "type": "Label", "text": "Search Mode:" },
                    "radio_normal": { "type": "RadioButton", "label": "Normal", "active": true },
                    "radio_extended": { "type": "RadioButton", "label": "Extended (\\n, \\r, \\t, \\0, \\x...)", "active": false },
                    "radio_regex": { "type": "RadioButton", "label": "Regular expression", "active": false }
                  }
                }
              }
            },
            "buttons": {
              "type": "VerticalLayout",
              "width": 300,
              "spacing": 4.0,
              "children": {
                "btn_find_current": { "type": "Button", "label": "Find All in Current Document", "width": -1 },
                "btn_find_all": { "type": "Button", "label": "Find All in All Opened Documents", "width": -1 },
                "count_row": {
                  "type": "HorizontalLayout",
                  "spacing": 4.0,
                  "children": {
                    "btn_count": { "type": "Button", "label": "Count", "width": -1 },
                    "btn_clear": { "type": "Button", "label": "Clear Results", "width": -1 }
                  }
                }
              }
            }
          }
        },
        "summary": { "type": "Label", "text": "" },
        "results_table": {
          "type": "Table", "id": "##nano_results", "columns": 3, "headers": true,
          "flags": "Resizable|RowBg|Borders|ScrollY", "height": -1,
          "children": {
            "col_file": { "type": "TableColumn", "label": "File", "flags": "WidthFixed", "init_width": 160 },
            "col_line": { "type": "TableColumn", "label": "Line", "flags": "WidthFixed", "init_width": 60 },
            "col_text": { "type": "TableColumn", "label": "Text", "flags": "WidthStretch" }
          }
        }
      }
    }
  }
})json";

// ── nano ───────────────────────────────────────────────────────────────────

nano::nano(dynamic&& base) : tool_form(std::move(base)) {}

nano::~nano() {
  remove_panel_objects();
}

void nano::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__nano_");
  search_root_key_ = internal_root_key_ + "_search";

  auto* title_f = findField<std::string>("title"_key);
  const std::string title = title_f ? *title_f : std::string{"Nano"};

  build_window(internal_root_key_, kToolbarLayout, window_id_, [&](ui_tree& tree) {
    tree.with("toolbar.btn_open", [&](const auto& e) { btn_open_id_ = wish_id_of(e); });
    tree.with("toolbar.btn_new", [&](const auto& e) { btn_new_id_ = wish_id_of(e); });
    tree.with("toolbar.btn_save", [&](const auto& e) { btn_save_id_ = wish_id_of(e); });
    tree.with("toolbar.btn_find", [&](const auto& e) { btn_find_id_ = wish_id_of(e); });
    tree.with("toolbar.current_label", [&](const auto& e) { current_label_ptr_ = e; });
  });

  build_window(search_root_key_, kSearchLayout, search_window_id_, [&](ui_tree& tree) {
    search_window_ptr_ = tree[""];
    tree.with("vbox.top.options.find_row.find_input", [&](const auto& e) {
      find_input_ptr_ = e;
      find_input_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.options.checks.chk_whole_word", [&](const auto& e) {
      chk_whole_word_ptr_ = e;
      chk_whole_word_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.options.checks.chk_match_case", [&](const auto& e) {
      chk_match_case_ptr_ = e;
      chk_match_case_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.options.mode_row.radio_normal", [&](const auto& e) {
      radio_normal_ptr_ = e;
      radio_normal_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.options.mode_row.radio_extended", [&](const auto& e) {
      radio_extended_ptr_ = e;
      radio_extended_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.options.mode_row.radio_regex", [&](const auto& e) {
      radio_regex_ptr_ = e;
      radio_regex_id_ = wish_id_of(e);
    });
    tree.with("vbox.top.buttons.btn_find_current", [&](const auto& e) { btn_find_current_id_ = wish_id_of(e); });
    tree.with("vbox.top.buttons.btn_find_all", [&](const auto& e) { btn_find_all_id_ = wish_id_of(e); });
    tree.with("vbox.top.buttons.count_row.btn_count", [&](const auto& e) { btn_count_id_ = wish_id_of(e); });
    tree.with("vbox.top.buttons.count_row.btn_clear", [&](const auto& e) { btn_clear_id_ = wish_id_of(e); });
    tree.with("vbox.summary", [&](const auto& e) { summary_label_ptr_ = e; });
    tree.with("vbox.results_table", [&](const auto& e) {
      results_table_ptr_ = e;
      results_table_id_ = wish_id_of(e);
    });
  });

  // Seed the first-run arrangement inside the editor's own nested dockspace
  // (titled with the form's "title" field): the Toolbar strip along the top
  // ~9%, the Search panel along the bottom ~36%, and the empty central
  // documents area between them that every file window docks into (see the
  // layout comment above). Owned by imgui.ini after the first run (see
  // docs/dock-layout.md); bump the version arg to layout() if it changes.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        kDockId, title,
        layout(
            split(
                dir::up, 0.09f, area({internal_root_key_}),
                split(dir::down, 0.36f, area({search_root_key_}), area({}))),
            /*version=*/1, /*target=*/kDockId)));
  }
}

void nano::remove_panel_objects() {
  // Keys are forgotten once removed: next_available_key() may hand this
  // form's freed internal_root_key_ to a new Nano instance, whose panels
  // would then reuse these exact secondary keys (see the same reasoning in
  // form::remove_objects_at()).
  for (auto& f : open_files_)
    remove_objects_at(f.root_key);
  open_files_.clear();
  current_index_ = -1;
  remove_objects_at(search_root_key_);
  search_root_key_.clear();
}

// ── open_file ─────────────────────────────────────────────────────────────────

dynamic nano::do_open_file(const dynamic& args) {
  auto path = args.as<std::string>("path"_key);
  std::string title = path;
  if (auto* t = args.findField<std::string>("title"_key); t && !t->empty())
    title = *t;

  auto resolved = file_service::resolve_path(path, sess().resource_dir, sess().allow_absolute_paths);
  if (resolved.empty()) {
    dynamic err;
    err["message"_key] = "invalid or unsafe path: " + path;
    emit("on_error"_key, std::move(err));
    return dynamic{};
  }

  // Already open: bring its window to the front instead.
  if (int existing = find_file_by_path(path); existing >= 0) {
    focus_file(static_cast<size_t>(existing));
    set_current(existing);
    return dynamic{};
  }

  const std::string initial_language = language_for_extension(path);
  open_file_entry entry;
  entry.path = path;
  entry.full_path = resolved.string();
  entry.title = title;
  entry.root_key = internal_root_key_ + "_doc_" + std::to_string(next_doc_seq_++);

  build_window(entry.root_key, kDocumentLayout, entry.window_id, [&](ui_tree& tree) {
    entry.window_ptr = tree[""];
    (*entry.window_ptr)["title"_key] = title;
    (*entry.window_ptr)["dock_target"_key] = std::string{kDockId};
    tree.with("vbox.lang", [&](const auto& e) {
      // Lets the user override the highlighting language the extension
      // guessed (or pick one at all for an unrecognized file).
      e["items"_key] = language_combo_items();
      e["value"_key] = language_index(initial_language);
      entry.lang_combo_id = wish_id_of(e);
    });
    tree.with("vbox.editor", [&](const auto& e) {
      e["file_path"_key] = path;
      e["language"_key] = initial_language;
      entry.editor_ptr = e;
      entry.editor_id = wish_id_of(e);
    });
  });

  open_files_.push_back(std::move(entry));
  set_current(static_cast<int>(open_files_.size()) - 1);

  dynamic opened;
  opened["path"_key] = path;
  opened["title"_key] = title;
  emit("on_file_opened"_key, std::move(opened));
  return dynamic{};
}

// ── File bookkeeping ──────────────────────────────────────────────────────────

int nano::find_file_by_widget(key_t id) const {
  for (size_t i = 0; i < open_files_.size(); ++i) {
    const auto& f = open_files_[i];
    if (f.window_id.id == id.id || f.editor_id.id == id.id || f.lang_combo_id.id == id.id)
      return static_cast<int>(i);
  }
  return -1;
}

int nano::find_file_by_path(const std::string& path) const {
  for (size_t i = 0; i < open_files_.size(); ++i)
    if (open_files_[i].path == path)
      return static_cast<int>(i);
  return -1;
}

void nano::update_window_title(size_t index) {
  auto& f = open_files_[index];
  if (f.window_ptr)
    (*f.window_ptr)["title"_key] = f.dirty ? (f.title + " *") : f.title;
}

void nano::set_current(int index) {
  current_index_ = index >= 0 && static_cast<size_t>(index) < open_files_.size() ? index : -1;
  if (current_label_ptr_)
    current_label_ptr_["text"_key] =
        current_index_ < 0 ? std::string{"No file open"} : "Current: " + open_files_[current_index_].title;
}

void nano::focus_file(size_t index) {
  auto& f = open_files_[index];
  if (f.window_ptr)
    (*f.window_ptr)["focus_request"_key] = f.window_ptr->get_as<int32_t>("focus_request"_key, 0) + 1;
}

void nano::save_file_at(size_t index) {
  open_files_[index].dirty = false;
  update_window_title(index);
  dynamic saved_payload;
  saved_payload["path"_key] = open_files_[index].path;
  emit("on_file_saved"_key, std::move(saved_payload));
}

void nano::close_file_at(size_t index) {
  if (index >= open_files_.size())
    return;
  auto entry = open_files_[index];
  remove_objects_at(entry.root_key);
  open_files_.erase(open_files_.begin() + static_cast<std::ptrdiff_t>(index));

  // The current document moves to the most recently opened remaining file
  // when it was the one closed; an index after it shifts down by one.
  if (current_index_ == static_cast<int>(index))
    set_current(static_cast<int>(open_files_.size()) - 1);
  else if (current_index_ > static_cast<int>(index))
    set_current(current_index_ - 1);

  dynamic payload;
  payload["path"_key] = entry.path;
  emit("on_file_closed"_key, std::move(payload));
}

// ── Search ────────────────────────────────────────────────────────────────────

void nano::set_search_mode(search_mode mode) {
  mode_ = mode;
  if (radio_normal_ptr_)
    radio_normal_ptr_["active"_key] = mode == search_mode::normal;
  if (radio_extended_ptr_)
    radio_extended_ptr_["active"_key] = mode == search_mode::extended;
  if (radio_regex_ptr_)
    radio_regex_ptr_["active"_key] = mode == search_mode::regex;
}

void nano::show_search_panel() {
  if (!search_window_ptr_)
    return;
  search_window_ptr_["visible"_key] = true;
  search_window_ptr_["focus_request"_key] = search_window_ptr_->get_as<int32_t>("focus_request"_key, 0) + 1;
}

int nano::run_search(bool all_files, bool count_only) {
  auto set_summary = [&](const std::string& text) {
    if (summary_label_ptr_)
      summary_label_ptr_["text"_key] = text;
  };

  const std::string typed = find_input_ptr_ ? find_input_ptr_->get_as<std::string>("value"_key, "") : "";
  const bool match_case = chk_match_case_ptr_ && chk_match_case_ptr_->get_as<bool>("value"_key, false);
  // Like Notepad++, "Match whole word only" does not apply to a regular
  // expression (write \b yourself).
  const bool whole_word =
      mode_ != search_mode::regex && chk_whole_word_ptr_ && chk_whole_word_ptr_->get_as<bool>("value"_key, false);

  if (typed.empty()) {
    set_summary("Type something to find.");
    return 0;
  }
  if (open_files_.empty()) {
    set_summary("No file open.");
    return 0;
  }
  if (!all_files && current_index_ < 0) {
    set_summary("No current document.");
    return 0;
  }

  std::optional<std::regex> re;
  std::string needle = typed;
  if (mode_ == search_mode::regex) {
    try {
      auto flags = std::regex::ECMAScript;
      if (!match_case)
        flags |= std::regex::icase;
      re.emplace(typed, flags);
    } catch (const std::regex_error& e) {
      set_summary(std::string{"Invalid regular expression: "} + e.what());
      return 0;
    }
  } else if (mode_ == search_mode::extended) {
    needle = unescape_extended(typed);
  }

  std::vector<size_t> targets;
  if (all_files) {
    for (size_t i = 0; i < open_files_.size(); ++i)
      targets.push_back(i);
  } else {
    targets.push_back(static_cast<size_t>(current_index_));
  }

  int total_hits = 0;
  size_t files_with_hits = 0;
  std::vector<search_hit> hits;
  for (size_t index : targets) {
    const auto& f = open_files_[index];
    // The TextEditor writes every edit straight through to the sandbox
    // file, so this is the live text, unsaved changes included.
    const std::string text = read_file(f.full_path);
    const auto matches = re ? find_regex(text, *re) : find_literal(text, needle, match_case, whole_word);
    if (matches.empty())
      continue;
    total_hits += static_cast<int>(matches.size());
    ++files_with_hits;
    if (count_only)
      continue;

    // One row per line, at its first match -- Notepad++'s results list
    // shows each matching line once.
    int32_t line = 1;
    size_t line_start = 0;
    size_t scanned = 0;
    int32_t last_line = 0;
    for (const auto& m : matches) {
      for (; scanned < m.pos; ++scanned) {
        if (text[scanned] == '\n') {
          ++line;
          line_start = scanned + 1;
        }
      }
      if (line == last_line)
        continue;
      last_line = line;
      size_t line_end = text.find('\n', line_start);
      if (line_end == std::string::npos)
        line_end = text.size();
      size_t match_end = std::min(m.pos + m.len, line_end);
      hits.push_back(
          {f.path, line, utf8_length(text, line_start, m.pos), utf8_length(text, m.pos, match_end)});
    }
  }

  std::ostringstream summary;
  if (count_only) {
    summary << "Count: " << total_hits << (total_hits == 1 ? " match" : " matches") << " in \""
            << open_files_[static_cast<size_t>(current_index_)].title << "\"";
    set_summary(summary.str());
    return total_hits;
  }

  search_hits_ = std::move(hits);
  if (search_hits_.size() > kMaxResultRows)
    search_hits_.resize(kMaxResultRows);
  summary << "Search \"" << typed << "\" (" << total_hits << (total_hits == 1 ? " hit" : " hits") << " in "
          << files_with_hits << (files_with_hits == 1 ? " file" : " files") << " of " << targets.size()
          << " searched)";
  if (search_hits_.size() == kMaxResultRows)
    summary << " -- showing the first " << kMaxResultRows << " lines";
  set_summary(summary.str());
  fill_results_table();
  return total_hits;
}

void nano::fill_results_table() {
  if (!results_table_ptr_)
    return;
  auto* children_p = results_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;
  // Drop the previous rows (numeric keys); the named TableColumn children
  // remain.
  children->clear();

  auto make_label = [](const std::string& text, int32_t order) {
    ui_element_ptr lbl = ui_element_ptr::create("wish"_key, "Label"_key);
    lbl["text"_key] = text;
    lbl["order"_key] = order;
    return lbl;
  };

  for (size_t i = 0; i < search_hits_.size(); ++i) {
    const auto& hit = search_hits_[i];
    int index = find_file_by_path(hit.path);
    std::string file_title = index >= 0 ? open_files_[static_cast<size_t>(index)].title : hit.path;

    // The line's own text, tabs as spaces, cut to a readable length.
    std::string line_text;
    if (index >= 0) {
      std::istringstream in(read_file(open_files_[static_cast<size_t>(index)].full_path));
      for (int32_t n = 0; n < hit.line && std::getline(in, line_text); ++n) {
      }
      if (!line_text.empty() && line_text.back() == '\r')
        line_text.pop_back();
      std::replace(line_text.begin(), line_text.end(), '\t', ' ');
      if (line_text.size() > kMaxResultTextLength)
        line_text = line_text.substr(0, kMaxResultTextLength) + "...";
    }

    ui_element_ptr row = ui_element_ptr::create("wish"_key, "TableRow"_key);
    row["order"_key] = static_cast<int32_t>(i);
    auto row_children = dynamic_ptr{key_t{0U}, {}};
    (*row_children)[size_t{0}] = dynamic_ptr{make_label(file_title, 0)};
    (*row_children)[size_t{1}] = dynamic_ptr{make_label(std::to_string(hit.line), 1)};
    (*row_children)[size_t{2}] = dynamic_ptr{make_label(line_text, 2)};
    row["children"_key] = row_children;
    row->refresh_children_order();
    (*children)[i] = dynamic_ptr{row};
  }
  results_table_ptr_->refresh_children_order();
}

void nano::go_to_hit(size_t row) {
  if (row >= search_hits_.size())
    return;
  const auto& hit = search_hits_[row];
  int index = find_file_by_path(hit.path);
  if (index < 0) {
    if (summary_label_ptr_)
      summary_label_ptr_["text"_key] = "\"" + hit.path + "\" is no longer open.";
    return;
  }
  auto& f = open_files_[static_cast<size_t>(index)];
  if (f.editor_ptr) {
    f.editor_ptr["goto_line"_key] = hit.line;
    f.editor_ptr["goto_column"_key] = hit.column;
    f.editor_ptr["goto_length"_key] = hit.length;
    f.editor_ptr["goto_request"_key] = f.editor_ptr->get_as<int32_t>("goto_request"_key, 0) + 1;
  }
  focus_file(static_cast<size_t>(index));
  set_current(index);
}

dynamic nano::do_find(const dynamic& args) {
  if (find_input_ptr_)
    find_input_ptr_["value"_key] = args.get_as<std::string>("text"_key, "");
  if (chk_match_case_ptr_)
    chk_match_case_ptr_["value"_key] = args.get_as<bool>("match_case"_key, false);
  if (chk_whole_word_ptr_)
    chk_whole_word_ptr_["value"_key] = args.get_as<bool>("whole_word"_key, false);
  const std::string mode = args.get_as<std::string>("mode"_key, "normal");
  set_search_mode(mode == "regex" ? search_mode::regex
                  : mode == "extended" ? search_mode::extended
                                       : search_mode::normal);
  int hits = run_search(args.get_as<std::string>("scope"_key, "current") == "all", /*count_only=*/false);
  dynamic result;
  result["hits"_key] = static_cast<int32_t>(hits);
  return result;
}

// ── Event routing ─────────────────────────────────────────────────────────────

void nano::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    bool any_dirty = std::any_of(open_files_.begin(), open_files_.end(), [](const auto& f) { return f.dirty; });
    if (!any_dirty) {
      do_close(/*flush_dirty_files=*/true);
      return;
    }

    // Don't close yet -- every panel stays open (nothing was removed from
    // top_level_objects) until confirm_close() answers this. Ask the client
    // which files, if any, should be saved first.
    dynamic paths;
    size_t i = 0;
    for (auto& f : open_files_)
      if (f.dirty)
        paths[i++] = f.path;
    dynamic confirm_payload;
    confirm_payload["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};
    emit("on_confirm_close"_key, std::move(confirm_payload));
    return;
  }

  // ── Toolbar ──
  if (event == "clicked"_key) {
    if (id == btn_open_id_) {
      emit("on_request_open"_key);
      return;
    }
    if (id == btn_new_id_) {
      emit("on_request_new"_key);
      return;
    }
    if (id == btn_save_id_) {
      if (current_index_ >= 0)
        save_file_at(static_cast<size_t>(current_index_));
      return;
    }
    if (id == btn_find_id_) {
      show_search_panel();
      return;
    }
  }

  // ── Search panel ──
  if (id == search_window_id_ && event == "closed"_key) {
    // Hide rather than tear down: the toolbar's Find brings it back with
    // its options and results intact.
    if (search_window_ptr_)
      search_window_ptr_["visible"_key] = false;
    return;
  }
  if (event == "clicked"_key) {
    if (id == radio_normal_id_) {
      set_search_mode(search_mode::normal);
      return;
    }
    if (id == radio_extended_id_) {
      set_search_mode(search_mode::extended);
      return;
    }
    if (id == radio_regex_id_) {
      set_search_mode(search_mode::regex);
      return;
    }
    if (id == btn_find_current_id_) {
      run_search(/*all_files=*/false, /*count_only=*/false);
      return;
    }
    if (id == btn_find_all_id_) {
      run_search(/*all_files=*/true, /*count_only=*/false);
      return;
    }
    if (id == btn_count_id_) {
      run_search(/*all_files=*/false, /*count_only=*/true);
      return;
    }
    if (id == btn_clear_id_) {
      search_hits_.clear();
      fill_results_table();
      if (summary_label_ptr_)
        summary_label_ptr_["text"_key] = std::string{};
      return;
    }
  }
  if (id == results_table_id_ && (event == "row_selected"_key || event == "row_activated"_key)) {
    int32_t row = payload.get_as<int32_t>("index"_key, -1);
    if (row >= 0)
      go_to_hit(static_cast<size_t>(row));
    return;
  }
  if (id == find_input_id_ || id == chk_whole_word_id_ || id == chk_match_case_id_)
    return; // their "value" fields are read when a search runs.

  // ── File windows ──
  int index = find_file_by_widget(id);
  if (index < 0)
    return;
  auto& f = open_files_[static_cast<size_t>(index)];

  if (id == f.window_id) {
    if (event == "closed"_key)
      close_file_at(static_cast<size_t>(index));
    else if (event == "focused"_key)
      set_current(index);
    return;
  }

  if (id == f.editor_id) {
    if (event == "changed"_key) {
      f.dirty = true;
      update_window_title(static_cast<size_t>(index));
    } else if (event == "saved"_key) {
      // Ctrl+S inside an editor: an explicit save of that one file.
      save_file_at(static_cast<size_t>(index));
    }
    return;
  }

  if (id == f.lang_combo_id && event == "changed"_key) {
    // Retargets the TextEditor's highlighting only -- not a content edit, so
    // this deliberately does not touch the file's dirty state.
    int32_t lang_idx = payload.get_as<int32_t>("value"_key, 0);
    if (lang_idx >= 0 && lang_idx < kLanguageCount && f.editor_ptr)
      f.editor_ptr["language"_key] = std::string{kLanguages[lang_idx]};
  }
}

dynamic nano::do_confirm_close(const dynamic& args) {
  do_close(args.as<bool>("save"_key));
  return dynamic{};
}

void nano::do_close(bool flush_dirty_files) {
  // Flush every eligible file before tearing down, so closing the whole
  // Nano has the same "download before discard" guarantee as closing one
  // file at a time -- except a file the user chose not to save is skipped so
  // its local copy is left untouched.
  for (auto& f : open_files_) {
    if (!flush_dirty_files && f.dirty)
      continue;
    dynamic closed_payload;
    closed_payload["path"_key] = f.path;
    emit("on_file_closed"_key, std::move(closed_payload));
  }

  emit("closed"_key);
  remove_panel_objects();
  remove_internal_objects();
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_nano() {
  auto proto = dynamic_ptr{"Nano"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Nano"},
          attr<DisplayName>("Title"),
          attr<Description>("Title of the editor's dockable tile (its nested dockspace)."),
          attr<Category>("Appearance")});

  proto->addMethod("open_file"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nano&>(self).do_open_file(args);
                   }});
  proto->addMethod("confirm_close"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nano&>(self).do_confirm_close(args);
                   }});
  proto->addMethod("find"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nano&>(self).do_find(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Nano"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Multi-file, syntax-highlighted text editor: a toolbar, one dockable window per "
                        "open file, and a Notepad++-style Search panel (find all in the current or "
                        "every open file; click a result to jump to it). Files live in the session "
                        "sandbox; the client must upload_file before calling open_file, and "
                        "download_file in response to on_file_closed or on_file_saved. If "
                        "on_confirm_close fires (the editor was closed with unsaved changes), ask "
                        "the user and call confirm_close(save). find(text, mode, match_case, "
                        "whole_word, scope) runs a search programmatically."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<nano>("wish"_key, "Nano"_key));
}

} // namespace bdg::wish
