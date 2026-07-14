// MIT License © 2025 Binary Dice Games
/// @file editor.cpp
/// @brief Implementation of the Editor form.
#include "editor.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/ui_importer.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

namespace {

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

// Event names emitted by wish's built-in element renderers (see
// src/imgui/imgui_*_renderer.cpp) aren't registered in bison's key-name
// registry (they're plain "_key" literals, not "_rkey"), so there is no
// generic hash -> string reversal available. This table covers the known
// vocabulary; anything else falls back to a numeric placeholder rather than
// silently dropping the log entry.
const std::unordered_map<uint32_t, std::string>& known_event_names() {
  static const std::unordered_map<uint32_t, std::string> table = {
      {"clicked"_key.id, "clicked"},
      {"changed"_key.id, "changed"},
      {"saved"_key.id, "saved"},
      {"closed"_key.id, "closed"},
      {"selected"_key.id, "selected"},
      {"sorted"_key.id, "sorted"},
      {"toggled"_key.id, "toggled"},
      {"row_selected"_key.id, "row_selected"},
      {"row_activated"_key.id, "row_activated"},
  };
  return table;
}

std::string event_name_string(key_t event) {
  auto& table = known_event_names();
  if (auto it = table.find(event.id); it != table.end())
    return it->second;
  return "event#" + std::to_string(event.id);
}

} // namespace

// ── UI layout ─────────────────────────────────────────────────────────────────

static constexpr const char* kEditorLayout = R"({
  "type": "Window",
  "title": "Editor",
  "width": 900,
  "height": 720,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "banner": { "type": "Label", "text": "" },
        "source": { "type": "TextEditor", "language": "json", "width": 0, "height": 440 },
        "log_label": { "type": "Label", "text": "Event Log" },
        "log": {
          "type": "Table",
          "id": "##editor_log",
          "columns": 2,
          "headers": true,
          "outer_height": 200,
          "children": {
            "col_seq":   { "type": "TableColumn", "label": "#" },
            "col_event": { "type": "TableColumn", "label": "Event" }
          }
        }
      }
    }
  }
})";

// ── editor ───────────────────────────────────────────────────────────────────

editor::editor(dynamic&& base) : form(std::move(base)) {}

void editor::on_init() {
  internal_root_key_ = "__editor_" + std::to_string(reinterpret_cast<uintptr_t>(this));
  mock_root_key_ = internal_root_key_ + "_mock";

  auto tree = import_json(kEditorLayout);

  // Assign every chrome element a bison RMI ID, mirroring calculator/notepad.
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }

  window_id_ = (*tree[""])["__wish_id"_key].as<key_t>();
  tree.with("vbox.source", [&](const auto& e) {
    source_editor_id_ = wish_id_of(e);
    source_editor_ptr_ = e;
  });
  tree.with("vbox.banner", [&](const auto& e) { banner_ptr_ = e; });
  tree.with("vbox.log", [&](const auto& e) { log_table_ptr_ = e; });

  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

// ── set_source / reparse ─────────────────────────────────────────────────────

dynamic editor::do_set_source(const dynamic& args) {
  current_source_path_ = args.as<std::string>("path"_key);
  if (source_editor_ptr_)
    (*source_editor_ptr_)["file_path"_key] = current_source_path_;
  try_reparse();
  return dynamic{};
}

void editor::with_session(const std::function<void(context&)>& fn) {
  if (detail::current_context) {
    // Called within dispatch (do_set_source): wlock already held.
    fn(*detail::current_context);
  } else {
    // Called from on_event(), which the render loop invokes outside the
    // session lock: acquire it ourselves.
    auto lock = context_wlock{*sync_ctx_};
    fn(*lock);
  }
}

void editor::try_reparse() {
  if (current_source_path_.empty())
    return;

  std::filesystem::path resolved;
  with_session([&](context& s) {
    resolved = file_service::resolve_path(current_source_path_, s.resource_dir, s.allow_absolute_paths);
  });
  if (resolved.empty()) {
    set_banner("invalid or unsafe source path: " + current_source_path_);
    return;
  }

  std::ifstream f(resolved, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>{}};

  ui_tree mock;
  try {
    mock = import_json(content);
  } catch (const std::runtime_error& e) {
    set_banner(e.what());
    return;
  }

  // Success: swap in the new preview subtree. Only reached once the new
  // tree has fully parsed, so a failed parse (handled above) never touches
  // the previous, still-valid preview -- this is what prevents flicker.
  clear_mock();

  auto& c = ctx();
  for (auto& [path, elem] : mock) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
    mock_id_to_path_[id.id] = path.empty() ? std::string{"root"} : path;
  }

  ui_element_ptr root_ptr = mock[""];
  with_session([&](context& s) {
    s.ui_objects.merge(std::move(mock), mock_root_key_);
    s.top_level_objects[key_t{mock_root_key_}] = root_ptr;
    s.top_level_handlers[key_t{mock_root_key_}] = this;
  });

  set_banner("");
}

void editor::clear_mock() {
  if (mock_root_key_.empty())
    return;
  with_session([&](context& s) {
    s.top_level_objects.erase(key_t{mock_root_key_});
    s.top_level_handlers.erase(key_t{mock_root_key_});
    const std::string dot = mock_root_key_ + ".";
    for (auto it = s.ui_objects.begin(); it != s.ui_objects.end();) {
      if (it->first == mock_root_key_ || it->first.rfind(dot, 0) == 0)
        it = s.ui_objects.erase(it);
      else
        ++it;
    }
  });
  mock_id_to_path_.clear();
}

void editor::set_banner(const std::string& text) {
  if (banner_ptr_)
    (*banner_ptr_)["text"_key] = text;
}

// ── Event log ─────────────────────────────────────────────────────────────────

void editor::append_log_row(const std::string& text) {
  if (!log_table_ptr_)
    return;
  auto* children_p = log_table_ptr_->findField<dynamic_ptr>("children"_key);
  if (!children_p || !*children_p)
    return;
  auto& children = *children_p;

  ui_element_ptr row{dynamic::instantiate("wish"_key, "TableRow"_key)};
  row["order"_key] = static_cast<int32_t>(next_log_child_key_);

  ui_element_ptr cell_seq{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_seq["text"_key] = std::to_string(++log_seq_);
  cell_seq["order"_key] = int32_t{0};
  key_t cell_seq_id = rmi::shared::generate_id();
  ctx().put_object(cell_seq_id, cell_seq);
  cell_seq["__wish_id"_key] = cell_seq_id;

  ui_element_ptr cell_text{dynamic::instantiate("wish"_key, "Label"_key)};
  cell_text["text"_key] = text;
  cell_text["order"_key] = int32_t{1};
  key_t cell_text_id = rmi::shared::generate_id();
  ctx().put_object(cell_text_id, cell_text);
  cell_text["__wish_id"_key] = cell_text_id;

  auto row_children = dynamic_ptr{key_t{0U}, {}};
  (*row_children)[size_t{0}] = dynamic_ptr{cell_seq};
  (*row_children)[size_t{1}] = dynamic_ptr{cell_text};
  row["children"_key] = row_children;
  row->refresh_children_order();

  key_t row_id = rmi::shared::generate_id();
  ctx().put_object(row_id, row);
  row["__wish_id"_key] = row_id;

  (*children)[next_log_child_key_++] = dynamic_ptr{row};
  log_table_ptr_->refresh_children_order();
}

// ── Event routing ─────────────────────────────────────────────────────────────

void editor::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == window_id_ && event == "closed"_key) {
    clear_mock();
    emit("closed"_key);
    remove_internal_objects();
    return;
  }

  if (id == source_editor_id_) {
    if (event == "changed"_key) {
      try_reparse();
      return;
    }
    if (event == "saved"_key) {
      emit("on_source_saved"_key);
      return;
    }
    return;
  }

  // Anything else fired within our subtrees is either a preview widget's
  // event -- log it -- or a stale id from a subtree that was just replaced
  // by try_reparse(), which is silently dropped.
  auto it = mock_id_to_path_.find(id.id);
  if (it == mock_id_to_path_.end())
    return;

  append_log_row(it->second + " " + event_name_string(event));
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_editor() {
  auto proto = dynamic_ptr{"Editor"_key, {}};

  proto->addMethod("set_source"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<editor&>(self).do_set_source(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Editor"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Live JSON UI mock editor: a syntax-highlighted source panel next to a "
                        "continuously re-parsed, live-instantiated preview and an event log. "
                        "The client owns the local JSON file; call set_source(path) after every "
                        "upload_file, both at startup and whenever the local file changes outside "
                        "the tool. Listen for 'closed' to detect when the user is done, and for "
                        "'on_source_saved' to persist Ctrl+S edits back to the local file."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<editor>("wish"_key, "Editor"_key));
}

} // namespace bdg::wish
