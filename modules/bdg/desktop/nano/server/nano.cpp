// MIT License © 2025 Binary Dice Games
/// @file nano.cpp
/// @brief Implementation of the Nano form.
#include "nano.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <cctype>
#include <filesystem>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

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
// above (the seeded default) and the per-tab language Combo's own item list,
// so the two never drift out of sync.
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

} // namespace

// ── Hardcoded UI layout ───────────────────────────────────────────────────────
//
// tab_bar's "children" is given explicitly (even though empty) so the
// importer allocates a private children map for this instance instead of
// falling back to the Element base prototype's shared default -- mutating
// that shared default would corrupt every TabBar in the process.
//
// tab_bar's "height": -1 is load-bearing, not cosmetic -- see
// modules/bdg/desktop/tail/server/tail.cpp's kLayout comment for the full
// mechanism. Each opened file's TextEditor (do_open_file() below) sets its
// own "height": 0 meaning "fill" per render_text_editor()'s own convention
// (imgui_text_editor_renderer.cpp treats width/height <= 0 as -1, ImGui's
// own "fill remaining region" sentinel) -- without tab_bar's own stretch
// hint, that fill-driven editor's real rendered height feeds back into
// vbox's own auto-sized natural height via tab_bar's last_rendered_size()
// fallback, compounding without bound frame over frame.
static constexpr const char* kNanoLayout = R"({
  "type": "Window",
  "title": "Nano",
  "width": 720, "height": 520,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 8.0,
          "children": {
            "btn_open": { "type": "Button", "label": "Open", "width": 90 },
            "btn_new":  { "type": "Button", "label": "New",  "width": 90 },
            "btn_save": { "type": "Button", "label": "Save", "width": 90 }
          }
        },
        "tab_bar": { "type": "TabBar", "id": "##nano_tabs", "height": -1, "children": {} }
      }
    }
  }
})";

// ── nano ───────────────────────────────────────────────────────────────────

nano::nano(dynamic&& base) : form(std::move(base)) {}

void nano::on_init() {
  // See form::internal_root_key_'s doc comment: ordinally-assigned, not pointer-derived.
  internal_root_key_ = next_available_key("__nano_");

  auto tree = import_json(kNanoLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Nano"};

  // Assign each imported element a bison RMI ID so the renderer can emit
  // events with the correct object ID. Mirrors the bc/file_dialog
  // pattern. put_object() files each one under the current request's group
  // (see rmi::context::current_group) so they're cleaned up together with
  // the rest of this form when relayed through rmi::bridge.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.toolbar.btn_open", [&](const auto& e) { btn_open_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_new", [&](const auto& e) { btn_new_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_save", [&](const auto& e) { btn_save_id_ = wish_id_of(e); });
  tree.with("vbox.tab_bar", [&](const auto& e) { tab_bar_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
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

  // Already open: no-op. TabItem has no server-settable "active" field, so
  // there is no way to force-focus the existing tab from here.
  for (auto& f : open_files_)
    if (f.path == path)
      return dynamic{};

  if (!tab_bar_ptr_)
    return dynamic{};
  auto* children_p = tab_bar_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return dynamic{};
  auto& children = *children_p;

  ui_element_ptr tab = ui_element_ptr::create("wish"_key, "TabItem"_key);
  tab["label"_key] = title;
  tab["closable"_key] = true;
  tab["order"_key] = static_cast<int32_t>(open_files_.size());

  std::string initial_language = language_for_extension(path);

  // Lets the user override the highlighting language the extension guessed
  // (or pick one at all for an extensionless/unrecognized file), independent
  // of the tab's own selected/dirty state.
  ui_element_ptr lang_combo = ui_element_ptr::create("wish"_key, "Combo"_key);
  lang_combo["label"_key] = std::string{"Language"};
  lang_combo["items"_key] = language_combo_items();
  lang_combo["value"_key] = language_index(initial_language);
  lang_combo["width"_key] = 150.0f;
  lang_combo["order"_key] = int32_t{0};

  ui_element_ptr editor{dynamic::instantiate("wish"_key, "TextEditor"_key)};
  editor["file_path"_key] = path;
  editor["language"_key] = initial_language;
  editor["width"_key] = int32_t{0}; // fill available width
  editor["height"_key] = int32_t{0}; // fill available height
  editor["order"_key] = int32_t{1};

  key_t tab_id = rmi::shared::generate_id();
  ctx().put_object(tab_id, tab);
  tab["__wish_id"_key] = tab_id;

  key_t lang_combo_id = rmi::shared::generate_id();
  ctx().put_object(lang_combo_id, lang_combo);
  lang_combo["__wish_id"_key] = lang_combo_id;

  key_t editor_id = rmi::shared::generate_id();
  ctx().put_object(editor_id, editor);
  editor["__wish_id"_key] = editor_id;

  // Give the TabItem its own private children map (see the layout comment
  // above) and place the language Combo and the TextEditor inside it.
  auto editor_children = dynamic_ptr{key_t{0U}, {}};
  (*editor_children)[size_t{0}] = dynamic_ptr{lang_combo};
  (*editor_children)[size_t{1}] = dynamic_ptr{editor};
  tab["children"_key] = editor_children;
  tab->refresh_children_order();

  size_t child_key = next_child_key_++;
  (*children)[child_key] = dynamic_ptr{tab};
  tab_bar_ptr_->refresh_children_order();

  // The first tab in a fresh TabBar is auto-selected by ImGui without ever
  // transitioning through a "was not selected" frame, so render_tab_item()'s
  // edge-triggered "selected" event (imgui_ui_renderer.cpp) never fires for
  // it -- track it as active here instead of waiting for that event.
  bool is_first_tab = open_files_.empty();
  open_files_.push_back({path, title, tab_id, editor_id, tab, child_key, /*dirty=*/false, editor, lang_combo_id});
  if (is_first_tab)
    active_tab_id_ = tab_id;
  rebuild_index_maps();

  dynamic opened;
  opened["path"_key] = path;
  opened["title"_key] = title;
  emit("on_file_opened"_key, std::move(opened));
  return dynamic{};
}

// ── Event routing ─────────────────────────────────────────────────────────────

void nano::on_event(key_t id, key_t event, const dynamic& payload) {
  if (id == window_id_ && event == "closed"_key) {
    bool any_dirty = false;
    for (auto& f : open_files_)
      if (f.dirty) {
        any_dirty = true;
        break;
      }

    if (!any_dirty) {
      do_close(/*flush_dirty_files=*/true);
      return;
    }

    // Don't close yet -- the window stays open (nothing was removed from
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

  if (id == btn_open_id_ && event == "clicked"_key) {
    emit("on_request_open"_key);
    return;
  }

  if (id == btn_new_id_ && event == "clicked"_key) {
    emit("on_request_new"_key);
    return;
  }

  if (id == btn_save_id_ && event == "clicked"_key) {
    auto it = tab_id_to_index_.find(active_tab_id_.id);
    if (active_tab_id_.id && it != tab_id_to_index_.end()) {
      size_t index = it->second;
      open_files_[index].dirty = false;
      update_tab_label(index);
      dynamic saved_payload;
      saved_payload["path"_key] = open_files_[index].path;
      emit("on_file_saved"_key, std::move(saved_payload));
    }
    return;
  }

  if (auto it = tab_id_to_index_.find(id.id); it != tab_id_to_index_.end()) {
    if (event == "closed"_key) {
      close_file_at(it->second);
      return;
    }
    if (event == "selected"_key) {
      active_tab_id_ = id;
      return;
    }
    return;
  }

  if (auto it = editor_id_to_index_.find(id.id); it != editor_id_to_index_.end()) {
    if (event == "changed"_key) {
      open_files_[it->second].dirty = true;
      update_tab_label(it->second);
      return;
    }
    if (event == "saved"_key) {
      // Ctrl+S inside a tab's TextEditor: forward as an explicit per-file save
      // signal.
      open_files_[it->second].dirty = false;
      update_tab_label(it->second);
      dynamic saved_payload;
      saved_payload["path"_key] = open_files_[it->second].path;
      emit("on_file_saved"_key, std::move(saved_payload));
    }
    return;
  }

  if (auto it = lang_combo_id_to_index_.find(id.id); it != lang_combo_id_to_index_.end() && event == "changed"_key) {
    // Retargets the TextEditor's highlighting only -- not a content edit, so
    // this deliberately does not touch the file's dirty state.
    int32_t lang_idx = payload.as<int32_t>("value"_key);
    if (lang_idx >= 0 && lang_idx < kLanguageCount) {
      auto& f = open_files_[it->second];
      if (f.editor_ptr)
        (*f.editor_ptr)["language"_key] = std::string{kLanguages[lang_idx]};
    }
  }
}

dynamic nano::do_confirm_close(const dynamic& args) {
  do_close(args.as<bool>("save"_key));
  return dynamic{};
}

void nano::do_close(bool flush_dirty_files) {
  // Flush every eligible file before tearing down, so closing the whole
  // Nano has the same "download before discard" guarantee as closing one
  // tab at a time -- except a file the user chose not to save is skipped so
  // its local copy is left untouched.
  for (auto& f : open_files_) {
    if (!flush_dirty_files && f.dirty)
      continue;
    dynamic closed_payload;
    closed_payload["path"_key] = f.path;
    emit("on_file_closed"_key, std::move(closed_payload));
  }
  open_files_.clear();
  tab_id_to_index_.clear();
  editor_id_to_index_.clear();
  active_tab_id_ = key_t{};

  emit("closed"_key);
  remove_internal_objects();
}

void nano::update_tab_label(size_t index) {
  auto& f = open_files_[index];
  if (f.tab_ptr)
    (*f.tab_ptr)["label"_key] = f.dirty ? (f.title + " *") : f.title;
}

void nano::close_file_at(size_t index) {
  if (index >= open_files_.size())
    return;
  auto entry = open_files_[index];

  if (active_tab_id_.id == entry.tab_id.id)
    active_tab_id_ = key_t{};

  if (tab_bar_ptr_) {
    if (auto* children_p = tab_bar_ptr_->findField<dynamic_ptr>("children"_key); children_p && *children_p)
      (*children_p)->erase(entry.child_key);
    tab_bar_ptr_->refresh_children_order();
  }

  open_files_.erase(open_files_.begin() + static_cast<std::ptrdiff_t>(index));
  rebuild_index_maps();

  dynamic payload;
  payload["path"_key] = entry.path;
  emit("on_file_closed"_key, std::move(payload));
}

void nano::rebuild_index_maps() {
  tab_id_to_index_.clear();
  editor_id_to_index_.clear();
  lang_combo_id_to_index_.clear();
  for (size_t i = 0; i < open_files_.size(); ++i) {
    tab_id_to_index_[open_files_[i].tab_id.id] = i;
    editor_id_to_index_[open_files_[i].editor_id.id] = i;
    lang_combo_id_to_index_[open_files_[i].lang_combo_id.id] = i;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_nano() {
  auto proto = dynamic_ptr{"Nano"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Nano"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addMethod("open_file"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nano&>(self).do_open_file(args);
                   }});
  proto->addMethod("confirm_close"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nano&>(self).do_confirm_close(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Nano"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Multi-file, syntax-highlighted text editor. Files live in the "
                        "session sandbox; the client must upload_file before calling "
                        "open_file, and download_file in response to on_file_closed or "
                        "on_file_saved. If on_confirm_close fires (the window was closed "
                        "with unsaved changes), ask the user and call confirm_close(save)."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<nano>("wish"_key, "Nano"_key));
}

} // namespace bdg::wish
