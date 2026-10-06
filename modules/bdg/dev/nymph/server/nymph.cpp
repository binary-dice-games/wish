// MIT License © 2026 Binary Dice Games
/// @file nymph.cpp
/// @brief Implementation of the Nymph form.
#include "nymph.hpp"

#include "nymph_csv.hpp"
#include "nymph_figure_renderer.hpp"
#include "nymph_png_meta.hpp"

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <context/file_service.hpp>
#include <ui/dock_layout_spec.hpp>
#include <ui/ui_importer.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

namespace {

/// Rows of the CSV the Data window shows at most.
constexpr size_t kMaxDataRows = 1000;

// The form's only auto-registered root. Never drawn (`visible: false`); it
// exists so the form has a top-level handler entry in both modes, which is
// what lets render() reach on_event() on the render thread.
constexpr const char* kHolderLayout = R"json({
  "type": "Window",
  "title": "nymph",
  "visible": false
})json";

// The Source window. "tabs" holds one file-backed TextEditor per part of the
// source; "format" gets YAML highlighting and wish's schema autocomplete.
constexpr const char* kSourceLayout = R"json({
  "type": "Window",
  "title": "Source",
  "width": 560,
  "height": 720,
  "closable": true,
  "children": {
    "vbox": {
      "type": "VerticalLayout",
      "children": {
        "toolbar": {
          "type": "HorizontalLayout",
          "spacing": 8,
          "children": {
            "save": { "type": "Button", "label": "Save" },
            "path_label": { "type": "Label", "text": "(no file)" }
          }
        },
        "banner": { "type": "Label", "text": "", "wrap": true, "text_color": "#D03030FF" },
        "tabs": {
          "type": "TabBar",
          "id": "##nymph_parts",
          "height": -1,
          "children": {
            "format_tab": {
              "type": "TabItem",
              "label": "Format",
              "children": {
                "format": { "type": "TextEditor", "language": "yaml", "wish_ui_schema": true, "width": 0, "height": 0 }
              }
            },
            "data_tab": {
              "type": "TabItem",
              "label": "Data",
              "children": {
                "data": { "type": "TextEditor", "language": "text", "width": 0, "height": 0 }
              }
            },
            "description_tab": {
              "type": "TabItem",
              "label": "Description",
              "children": {
                "description": { "type": "TextEditor", "language": "text", "width": 0, "height": 0 }
              }
            }
          }
        }
      }
    }
  }
})json";

template <typename Element>
key_t wish_id_of(const Element& element) {
  return element->template as<key_t>("__wish_id"_key);
}

dynamic node(const char* type) {
  dynamic out;
  out["__type__"_key] = key_t{type};
  return out;
}

/// Adds @p child to @p children under @p name, in declaration order.
void add_named(dynamic_ptr& children, const std::string& name, dynamic child, int32_t order) {
  child["__name__"_key] = name;
  child["order"_key] = order;
  (*children)[key_t{name}] = dynamic_ptr{std::move(child)};
}

void add_indexed(dynamic_ptr& children, size_t index, dynamic child) {
  child["order"_key] = static_cast<int32_t>(index);
  (*children)[index] = dynamic_ptr{std::move(child)};
}

dynamic_ptr new_children() {
  return dynamic_ptr{key_t{0U}, {}};
}

std::string without_trailing_newlines(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
}

/// An error message for edit mode, where each part has its own editor: the
/// line is given within the part the error is in, not within the whole source.
std::string locate(const nymph::error& e, const nymph::document& doc) {
  if (e.line() >= doc.data_line)
    return "Data, line " + std::to_string(e.line() - doc.data_line + 1) + ": " + e.message();
  if (e.line() >= doc.format_line)
    return "Format, line " + std::to_string(e.line() - doc.format_line + 1) + ": " + e.message();
  return e.message();
}

} // namespace

// ── construction ─────────────────────────────────────────────────────────────

nymph_form::nymph_form(dynamic&& base) : form(std::move(base)) {}

void nymph_form::on_init() {
  internal_root_key_ = next_available_key("__nymph_");
  source_root_key_ = internal_root_key_ + "_source";
  preview_root_key_ = internal_root_key_ + "_preview";
  data_root_key_ = internal_root_key_ + "_data";

  auto tree = import_json(kHolderLayout);
  holder_id_ = rmi::shared::generate_id();
  ctx().put_object(holder_id_, tree[""]);
  (*tree[""])["__wish_id"_key] = holder_id_;
  sess().ui_objects.merge(std::move(tree), internal_root_key_);
}

void nymph_form::on_construct(const dynamic& params) {
  if (auto* silent = params.findField<bool>("silent"_key))
    silent_ = *silent;
  if (auto* view = params.findField<bool>("view"_key))
    view_ = *view;
}

void nymph_form::with_session(const std::function<void(context&)>& fn) {
  if (detail::current_context) {
    fn(*detail::current_context); // within dispatch: the write lock is already held
  } else {
    auto lock = context_wlock{*sync_ctx_};
    fn(*lock);
  }
}

// ── sandbox files ────────────────────────────────────────────────────────────

std::string nymph_form::read_sandbox_file(const std::string& name) {
  std::filesystem::path resolved;
  with_session([&](context& s) {
    resolved = file_service::resolve_path(name, s.resource_dir, s.allow_absolute_paths);
  });
  if (resolved.empty())
    throw std::runtime_error("invalid or unsafe path: " + name);
  std::ifstream in(resolved, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read " + name);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

void nymph_form::write_sandbox_file(const std::string& name, const std::string& bytes) {
  std::filesystem::path resolved;
  with_session([&](context& s) {
    resolved = file_service::resolve_path(name, s.resource_dir, s.allow_absolute_paths);
  });
  if (resolved.empty())
    throw std::runtime_error("invalid or unsafe path: " + name);
  std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out)
    throw std::runtime_error("cannot write " + name);
}

// ── load / source ────────────────────────────────────────────────────────────

dynamic nymph_form::do_load(const dynamic& args) {
  const std::string path = args.as<std::string>("path"_key);
  if (auto* dp = args.findField<std::string>("display_path"_key); dp && !dp->empty())
    display_path_ = *dp;
  bool validate = true;
  if (auto* v = args.findField<bool>("validate"_key))
    validate = *v;

  std::string bytes = read_sandbox_file(path);
  std::string load_error;
  std::string text;
  if (nymph::is_png(bytes)) {
    if (auto embedded = nymph::read_source(bytes))
      text = std::move(*embedded);
    else
      load_error = "not a nymph image (the PNG has no embedded nymph source)";
  } else {
    text = std::move(bytes);
  }

  if (text_backed()) {
    if (!load_error.empty())
      throw std::runtime_error(load_error);
    // View mode has nothing to show for a source that does not bind, so it
    // always validates, and fails the same way silent mode does.
    if (validate || view_) {
      nymph::document doc = nymph::parse_document(text);
      figure_ = nymph::bind(doc);
      document_ = std::move(doc);
    } else {
      figure_.reset();
    }
    loaded_ = true;
    source_text_ = std::move(text);
    if (view_)
      show_view();
    return dynamic{};
  }

  // Edit mode: a source that does not load is shown and reported, never
  // thrown, so the user can fix it in the editors.
  nymph::document doc;
  if (load_error.empty()) {
    try {
      doc = nymph::parse_document(text);
    } catch (const nymph::error& e) {
      load_error = e.message();
      doc = nymph::make_document(text, "", "");
    }
  }
  if (!ui_built_)
    build_edit_ui();
  document_ = doc;
  loaded_ = true;
  dirty_ = false;
  show_document(doc);
  update_path_label();
  rebind();
  if (!load_error.empty())
    set_banner(load_error);
  return dynamic{};
}

nymph::document nymph_form::current_document() {
  if (text_backed())
    return document_;
  nymph::document doc = nymph::make_document(
      read_sandbox_file(description_file_), read_sandbox_file(format_file_), read_sandbox_file(data_file_));
  // Keep the loaded file's own separator lines (line ending, trailing
  // blanks), so saving an untouched source reproduces it byte for byte.
  doc.separator1 = document_.separator1;
  doc.separator2 = document_.separator2;
  return doc;
}

std::string nymph_form::current_source() {
  return text_backed() ? source_text_ : nymph::compose_document(current_document());
}

dynamic nymph_form::do_source(const dynamic& /*args*/) {
  if (!loaded_)
    throw std::runtime_error("nothing is loaded");
  const std::string name = "nymph_source_" + std::to_string(file_counter_++) + ".txt";
  write_sandbox_file(name, current_source());
  dynamic out;
  out["path"_key] = name;
  return out;
}

// ── render ───────────────────────────────────────────────────────────────────

dynamic nymph_form::do_render(const dynamic& /*args*/) {
  if (!loaded_)
    throw std::runtime_error("nothing is loaded");
  // Not rendered here: this is an RMI dispatch thread, and the rasterizer
  // must run on the render thread. A self-addressed event gets there the
  // same way a button click does (see on_event()).
  sess().pending_events.push_back({holder_id_, "render_requested"_key, dynamic{}, key_t{internal_root_key_}});
  return dynamic{};
}

bool nymph_form::render_to_sandbox(key_t done_event) {
  std::string failure;
  try {
    std::string source;
    std::string description;
    nymph::figure rebound;
    const nymph::figure* fig = nullptr;
    if (text_backed()) {
      if (!figure_)
        throw std::runtime_error("the loaded source was not validated");
      source = source_text_;
      description = document_.description;
      fig = &*figure_;
    } else {
      nymph::document doc = current_document();
      try {
        rebound = nymph::bind(doc);
      } catch (const nymph::error& e) {
        throw std::runtime_error(locate(e, doc));
      }
      source = nymph::compose_document(doc);
      description = doc.description;
      fig = &rebound;
    }

    name_map names;
    ui_element_ptr root = build_ui_node(fig->root, "", false, names);
    nymph::image img;
    with_session([&](context& s) { img = nymph::render_figure(*root, s, fig->options); });

    const std::string name = "nymph_out_" + std::to_string(file_counter_++) + ".png";
    write_sandbox_file(name, nymph::encode_png(img, source, without_trailing_newlines(description)));
    dynamic payload;
    payload["path"_key] = name;
    emit(done_event, std::move(payload));
    return true;
  } catch (const std::exception& e) {
    failure = e.what();
  }
  if (!silent_)
    set_banner("Not saved: " + failure);
  dynamic payload;
  payload["message"_key] = failure;
  emit("render_failed"_key, std::move(payload));
  return false;
}

// ── edit mode: windows ───────────────────────────────────────────────────────

void nymph_form::build_edit_ui() {
  auto tree = import_json(kSourceLayout);
  auto& c = ctx();
  for (auto& [key, elem] : tree) {
    key_t id = rmi::shared::generate_id();
    c.put_object(id, elem);
    elem["__wish_id"_key] = id;
  }
  source_window_id_ = wish_id_of(tree[""]);
  tree.with("vbox.toolbar.save", [&](const auto& e) { save_button_id_ = wish_id_of(e); });
  tree.with("vbox.toolbar.path_label", [&](const auto& e) { path_label_ = e; });
  tree.with("vbox.banner", [&](const auto& e) { banner_ = e; });
  tree.with("vbox.tabs.format_tab.format", [&](const auto& e) {
    format_editor_ = e;
    format_editor_id_ = wish_id_of(e);
  });
  tree.with("vbox.tabs.data_tab.data", [&](const auto& e) {
    data_editor_ = e;
    data_editor_id_ = wish_id_of(e);
  });
  tree.with("vbox.tabs.description_tab.description", [&](const auto& e) {
    description_editor_ = e;
    description_editor_id_ = wish_id_of(e);
  });

  // A second top-level root next to the invisible holder; registered by
  // hand, the way the editor module registers its Help window.
  ui_element_ptr root = tree[""];
  with_session([&](context& s) {
    s.ui_objects.merge(std::move(tree), source_root_key_);
    s.top_level_objects[key_t{source_root_key_}] = root;
    s.top_level_handlers[key_t{source_root_key_}] = this;
  });
  (*root)["__path__"_key] = source_root_key_;

  // First-run arrangement (owned by imgui.ini afterwards): the source on the
  // left, the preview over the data table on the right.
  {
    using namespace dock;
    set_default_dock_layout(viewport(
        "nymph_dock", "Nymph",
        layout(
            split(
                dir::left, 0.38f,
                area({source_root_key_}),
                split(dir::down, 0.30f, area({data_root_key_}), area({preview_root_key_}))),
            /*version=*/1, /*target=*/"nymph_dock")));
  }
  ui_built_ = true;
}

void nymph_form::show_document(const nymph::document& doc) {
  // Fresh names every time: a TextEditor only reloads its buffer when its
  // file_path changes, not when the file under an unchanged path does.
  const std::string stem = "nymph_" + std::to_string(file_counter_++);
  description_file_ = stem + "_description.txt";
  format_file_ = stem + "_format.yaml";
  data_file_ = stem + "_data.csv";
  write_sandbox_file(description_file_, doc.description);
  write_sandbox_file(format_file_, doc.format);
  write_sandbox_file(data_file_, doc.data);
  if (description_editor_)
    (*description_editor_)["file_path"_key] = description_file_;
  if (format_editor_)
    (*format_editor_)["file_path"_key] = format_file_;
  if (data_editor_)
    (*data_editor_)["file_path"_key] = data_file_;
  data_shown_ = false;
}

void nymph_form::rebind() {
  nymph::document doc;
  try {
    doc = current_document();
  } catch (const std::exception& e) {
    bind_ok_ = false;
    set_banner(e.what());
    return;
  }

  // The Data window follows the CSV whenever the CSV itself parses, even
  // while the format part is mid-edit and does not bind.
  if (!data_shown_ || doc.data != shown_data_) {
    try {
      rebuild_data(nymph::parse_csv(doc.data, doc.data_line));
      shown_data_ = doc.data;
      data_shown_ = true;
    } catch (const nymph::error&) {
      // Reported by bind() below; the previous table stays.
    }
  }

  try {
    nymph::figure fig = nymph::bind(doc);
    // Only reached once the new source is fully valid, so a half-typed edit
    // never blanks the previous, still-valid preview.
    rebuild_preview(fig);
    bind_ok_ = true;
    set_banner("");
  } catch (const nymph::error& e) {
    bind_ok_ = false;
    set_banner(locate(e, doc));
  } catch (const std::exception& e) {
    bind_ok_ = false;
    set_banner(e.what());
  }
}

void nymph_form::clear_window(const std::string& root_key) {
  with_session([&](context& s) {
    s.top_level_objects.erase(key_t{root_key});
    s.top_level_handlers.erase(key_t{root_key});
    const std::string dot = root_key + ".";
    for (auto it = s.ui_objects.begin(); it != s.ui_objects.end();) {
      if (it->first == root_key || it->first.rfind(dot, 0) == 0)
        it = s.ui_objects.erase(it);
      else
        ++it;
    }
  });
}

void nymph_form::replace_window(const std::string& root_key, key_t& window_id, const dynamic& root) {
  ui_tree tree;
  ui_element_ptr root_ptr = build_ui_node(root, "", true, tree);
  for (auto& [path, elem] : tree) {
    // The window keeps its id across rebuilds: ImGui keys a window's
    // position, size, dock and focus off it. Everything inside is new.
    key_t id = rmi::shared::generate_id();
    if (path.empty()) {
      if (window_id.id)
        id = window_id;
      else
        window_id = id;
    }
    elem["__wish_id"_key] = id;
  }
  (*root_ptr)["__path__"_key] = root_key;

  clear_window(root_key);
  with_session([&](context& s) {
    s.ui_objects.merge(std::move(tree), root_key);
    s.top_level_objects[key_t{root_key}] = root_ptr;
    s.top_level_handlers[key_t{root_key}] = this;
  });
}

void nymph_form::show_view() {
  // The figure kept for render() stays whole; the preview gets its own
  // bound copy, since rebuild_preview() moves the descriptor into the window.
  nymph::figure shown = nymph::bind(document_);
  rebuild_preview(shown);
  rebuild_data(shown.data);
  if (view_built_)
    return;

  // First-run arrangement: the plot over its data. Its own dock id, so the
  // arrangement saved for view mode never fights edit mode's.
  using namespace dock;
  set_default_dock_layout(viewport(
      "nymph_view_dock", "Nymph",
      layout(
          split(dir::down, 0.28f, area({data_root_key_}), area({preview_root_key_})),
          /*version=*/1, /*target=*/"nymph_view_dock")));
  view_built_ = true;
}

void nymph_form::rebuild_preview(nymph::figure& fig) {
  dynamic window = node("Window");
  window["title"_key] = std::string{"Preview"};
  // View mode has no Source window; its Preview is the one to close.
  if (view_)
    window["closable"_key] = true;
  // Sized so the plot appears at about the size it will have in the image.
  window["width"_key] = fig.options.width + 2 * fig.options.padding + 16;
  window["height"_key] = fig.options.height + 2 * fig.options.padding + 40;
  auto children = new_children();
  add_named(children, "figure", std::move(fig.root), 0);
  window["children"_key] = children;
  replace_window(preview_root_key_, preview_window_id_, window);
}

void nymph_form::rebuild_data(const nymph::table& data) {
  const size_t shown = std::min(data.row_count(), kMaxDataRows);

  dynamic status = node("Label");
  status["text"_key] = data.headers.empty()
                           ? std::string{"No data."}
                           : "Showing " + std::to_string(shown) + " of " + std::to_string(data.row_count()) + " rows";

  dynamic table = node("Table");
  table["id"_key] = std::string{"##nymph_data"};
  table["columns"_key] = static_cast<int32_t>(std::max<size_t>(data.headers.size(), 1));
  table["headers"_key] = true;
  table["outer_height"_key] = -1.0f;
  table["height"_key] = -1.0f; // layout hint: the stretch row of the VerticalLayout below
  table["auto_scroll"_key] = false;
  table["flags"_key] = std::string{"ScrollY|RowBg|BordersV|Resizable"};
  auto rows = new_children();
  size_t index = 0;
  for (size_t c = 0; c < data.headers.size(); ++c) {
    dynamic column = node("TableColumn");
    column["label"_key] = data.headers[c];
    add_indexed(rows, index++, std::move(column));
  }
  for (size_t r = 0; r < shown; ++r) {
    dynamic row = node("TableRow");
    auto cells = new_children();
    for (size_t c = 0; c < data.headers.size(); ++c) {
      dynamic cell = node("Label");
      cell["text"_key] = data.columns[c][r];
      add_indexed(cells, c, std::move(cell));
    }
    row["children"_key] = cells;
    add_indexed(rows, index++, std::move(row));
  }
  table["children"_key] = rows;

  dynamic vbox = node("VerticalLayout");
  auto vbox_children = new_children();
  add_named(vbox_children, "status", std::move(status), 0);
  add_named(vbox_children, "table", std::move(table), 1);
  vbox["children"_key] = vbox_children;

  dynamic window = node("Window");
  window["title"_key] = std::string{"Data"};
  window["width"_key] = 640;
  window["height"_key] = 300;
  auto window_children = new_children();
  add_named(window_children, "vbox", std::move(vbox), 0);
  window["children"_key] = window_children;
  replace_window(data_root_key_, data_window_id_, window);
}

void nymph_form::set_banner(const std::string& text) {
  if (banner_)
    (*banner_)["text"_key] = text;
}

void nymph_form::update_path_label() {
  if (!path_label_)
    return;
  std::string text = display_path_.empty() ? std::string{"(no file)"} : display_path_;
  if (dirty_)
    text += " [MODIFIED]";
  (*path_label_)["text"_key] = text;
}

// ── edit mode: save / close ──────────────────────────────────────────────────

dynamic nymph_form::do_mark_saved(const dynamic& /*args*/) {
  dirty_ = false;
  update_path_label();
  if (pending_close_after_save_) {
    pending_close_after_save_ = false;
    request_close();
  }
  return dynamic{};
}

void nymph_form::show_close_confirm() {
  dynamic params;
  params["title"_key] = std::string{"Unsaved changes"};
  params["message"_key] =
      "Save changes to " + (display_path_.empty() ? std::string{"this image"} : display_path_) + " before closing?";
  params["icon"_key] = std::string{"warning"};
  params["buttons"_key] = std::string{"yes_no_cancel"};

  // Same flow as the editor module's close dialog. The callback must not
  // destroy close_dialog_: the MessageBox closes itself right after
  // emitting "on_result", so freeing it here would be a use-after-free.
  close_dialog_ = instantiate_child_form<message_box>(
      "MessageBox"_key, std::move(params), [this](key_t /*event_name*/, const dynamic& payload) {
        const std::string button = payload.as<std::string>("button"_key);
        if (button == "yes") {
          // Closes once the client reports the image landed (do_mark_saved).
          // A source that does not render keeps the window open.
          pending_close_after_save_ = render_to_sandbox("on_image_saved"_key);
        } else if (button == "no") {
          request_close();
        }
      });
}

void nymph_form::request_close() {
  if (ui_built_ || view_built_) {
    clear_window(preview_root_key_);
    clear_window(data_root_key_);
  }
  view_built_ = false;
  emit("closed"_key);
  remove_internal_objects();
  if (ui_built_)
    remove_objects_at(source_root_key_);
  ui_built_ = false;
}

// ── events ───────────────────────────────────────────────────────────────────

void nymph_form::on_event(key_t id, key_t event, const dynamic& /*payload*/) {
  if (id == holder_id_) {
    if (event == "render_requested"_key)
      render_to_sandbox("rendered"_key);
    return;
  }
  if (view_built_ && id == preview_window_id_ && event == "closed"_key) {
    request_close(); // nothing can be unsaved in view mode
    return;
  }
  if (!ui_built_)
    return;

  if (id == source_window_id_ && event == "closed"_key) {
    if (dirty_)
      show_close_confirm();
    else
      request_close();
    return;
  }
  if (id == save_button_id_ && event == "clicked"_key) {
    render_to_sandbox("on_image_saved"_key);
    return;
  }
  if (id == format_editor_id_ || id == data_editor_id_ || id == description_editor_id_) {
    if (event == "changed"_key) {
      dirty_ = true;
      update_path_label();
      rebind();
    } else if (event == "saved"_key) { // Ctrl+S inside an editor
      render_to_sandbox("on_image_saved"_key);
    }
  }
}

// ── registration ─────────────────────────────────────────────────────────────

void register_nymph() {
  auto proto = dynamic_ptr{"Nymph"_key, {}};

  // Without __construct, the params given to instantiate() would be
  // dropped: on_init() runs before they are otherwise applied.
  proto->addMethod("__construct"_key, bison::method{[](dynamic& self, const dynamic& params) -> dynamic {
                     static_cast<nymph_form&>(self).on_construct(params);
                     return dynamic{};
                   }});
  proto->addMethod("load"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nymph_form&>(self).do_load(args);
                   }});
  proto->addMethod("render"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nymph_form&>(self).do_render(args);
                   }});
  proto->addMethod("source"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nymph_form&>(self).do_source(args);
                   }});
  proto->addMethod("mark_saved"_key, bison::method{[](dynamic& self, const dynamic& args) -> dynamic {
                     return static_cast<nymph_form&>(self).do_mark_saved(args);
                   }});

  (*proto)[dynamic::CLASS].addAttribute(attr<DisplayName>("Nymph"));
  (*proto)[dynamic::CLASS].addAttribute(
      attr<Description>("Chart-from-text tool: renders a nymph source (description, format YAML, data CSV) "
                        "to a PNG with wish's Plot/Plot3D widgets and embeds the source in the image. "
                        "Construct with {silent: true} for no UI, or {view: true} for a read-only "
                        "preview and data table. Upload the source text or a nymph PNG, "
                        "call load({path, display_path}), then render() and wait for 'rendered' {path} "
                        "(or 'render_failed' {message}) and download that file. In edit mode, listen for "
                        "'on_image_saved' {path}: download it, store it locally and call mark_saved(). "
                        "Listen for 'closed' to know when the user is done."));

  dynamic::addClass(
      "wish"_key, std::move(proto), key_t{0U}, dynamic::make_factory<nymph_form>("wish"_key, "Nymph"_key));
}

} // namespace bdg::wish
