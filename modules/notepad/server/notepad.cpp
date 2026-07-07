// MIT License © 2025 Binary Dice Games
/// @file notepad.cpp
/// @brief Implementation of the Notepad form.
#include "notepad.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <file_service.hpp>
#include <ui_importer.hpp>

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
// values (see src/ui_elements/text_editor.cpp). Unknown extensions fall back
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

} // namespace

// ── Hardcoded UI layout ───────────────────────────────────────────────────────
//
// tab_bar's "children" is given explicitly (even though empty) so the
// importer allocates a private children map for this instance instead of
// falling back to the Element base prototype's shared default -- mutating
// that shared default would corrupt every TabBar in the process.
static constexpr const char* kNotepadLayout = R"({
  "type": "Window",
  "title": "Notepad",
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
            "btn_sync": { "type": "Button", "label": "Sync", "width": 90 }
          }
        },
        "tab_bar": { "type": "TabBar", "id": "##notepad_tabs", "children": {} }
      }
    }
  }
})";

// ── notepad ───────────────────────────────────────────────────────────────────

notepad::notepad(dynamic&& base) : form(std::move(base)) {}

void notepad::on_init() {
  internal_root_key_ = "__notepad_" + std::to_string(reinterpret_cast<uintptr_t>(this));

  auto tree = import_json(kNotepadLayout);

  auto* title_f = findField<std::string>("title"_key);
  (*tree[""])["title"_key] = title_f ? *title_f : std::string{"Notepad"};

  // Assign each imported element a bison RMI ID so the renderer can emit
  // events with the correct object ID. Mirrors the calculator/file_dialog
  // pattern.
  auto& objects = ctx().objects;
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    objects[id.id] = elem;
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.toolbar.btn_open", [&](const auto& e) { btn_open_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_new", [&](const auto& e) { btn_new_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.btn_sync", [&](const auto& e) { btn_sync_id_ = wish_id_of(e); });
  tree.with("vbox.tab_bar", [&](const auto& e) { tab_bar_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

// ── open_file ─────────────────────────────────────────────────────────────────

dynamic notepad::do_open_file(const dynamic& args) {
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

  ui_element_ptr tab{dynamic::instantiate("wish"_key, "TabItem"_key)};
  tab["label"_key] = title;
  tab["closable"_key] = true;
  tab["order"_key] = static_cast<int32_t>(open_files_.size());

  ui_element_ptr editor{dynamic::instantiate("wish"_key, "TextEditor"_key)};
  editor["file_path"_key] = path;
  editor["language"_key] = language_for_extension(path);
  editor["width"_key] = int32_t{0}; // fill available width
  editor["height"_key] = int32_t{0}; // fill available height
  editor["order"_key] = int32_t{0};

  key_t tab_id = rmi::shared::generate_id();
  ctx().objects[tab_id.id] = tab;
  tab["__wish_id"_key] = tab_id;

  key_t editor_id = rmi::shared::generate_id();
  ctx().objects[editor_id.id] = editor;
  editor["__wish_id"_key] = editor_id;

  // Give the TabItem its own private children map (see the layout comment
  // above) and place the TextEditor inside it.
  auto editor_children = dynamic_ptr{key_t{0U}, {}};
  (*editor_children)[size_t{0}] = dynamic_ptr{editor};
  tab["children"_key] = editor_children;
  tab->refresh_children_order();

  size_t child_key = next_child_key_++;
  (*children)[child_key] = dynamic_ptr{tab};
  tab_bar_ptr_->refresh_children_order();

  open_files_.push_back({path, title, tab_id, editor_id, tab, child_key});
  rebuild_index_maps();

  dynamic opened;
  opened["path"_key] = path;
  opened["title"_key] = title;
  emit("on_file_opened"_key, std::move(opened));
  return dynamic{};
}

// ── Event routing ─────────────────────────────────────────────────────────────

void notepad::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == window_id_ && event == "closed"_key) {
    // Flush every remaining file before tearing down, so closing the whole
    // Notepad has the same "download before discard" guarantee as closing
    // one tab at a time.
    for (auto& f : open_files_) {
      dynamic closed_payload;
      closed_payload["path"_key] = f.path;
      emit("on_file_closed"_key, std::move(closed_payload));
    }
    open_files_.clear();
    tab_id_to_index_.clear();
    editor_id_to_index_.clear();

    emit("closed"_key);
    remove_internal_objects();
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

  if (id == btn_sync_id_ && event == "clicked"_key) {
    dynamic paths;
    size_t i = 0;
    for (auto& f : open_files_)
      paths[i++] = f.path;
    dynamic sync_payload;
    sync_payload["paths"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(paths))};
    emit("on_sync_requested"_key, std::move(sync_payload));
    return;
  }

  if (auto it = tab_id_to_index_.find(id.id); it != tab_id_to_index_.end() && event == "closed"_key) {
    close_file_at(it->second);
    return;
  }

  if (auto it = editor_id_to_index_.find(id.id); it != editor_id_to_index_.end() && event == "saved"_key) {
    // Ctrl+S inside a tab's TextEditor: forward as an explicit per-file save
    // signal. TextEditor's "changed" event is not forwarded -- it only marks
    // that the widget already auto-persisted to the sandbox file.
    dynamic saved_payload;
    saved_payload["path"_key] = open_files_[it->second].path;
    emit("on_file_saved"_key, std::move(saved_payload));
  }
}

void notepad::close_file_at(size_t index) {
  if (index >= open_files_.size())
    return;
  auto entry = open_files_[index];

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

void notepad::rebuild_index_maps() {
  tab_id_to_index_.clear();
  editor_id_to_index_.clear();
  for (size_t i = 0; i < open_files_.size(); ++i) {
    tab_id_to_index_[open_files_[i].tab_id.id] = i;
    editor_id_to_index_[open_files_[i].editor_id.id] = i;
  }
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_notepad() {
  auto proto = dynamic_ptr{"Notepad"_key, {}};

  proto->addField(
      "title"_key,
      field{
          std::string{"Notepad"},
          attr<DisplayName>("Title"),
          attr<Description>("Window title."),
          attr<Category>("Appearance")});

  proto->addMethod("open_file"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<notepad&>(self).do_open_file(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Notepad"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Multi-file, syntax-highlighted text editor. Files live in the "
                        "session sandbox; the client must upload_file before calling "
                        "open_file, and download_file in response to on_file_closed, "
                        "on_file_saved, or on_sync_requested."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<notepad>("wish"_key, "Notepad"_key));
}

} // namespace bdg::wish
