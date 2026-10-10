// MIT License © 2026 Binary Dice Games
/// @file text_viewer_panel.cpp
/// @brief Implementation of common::text_viewer_panel.
#include "modules/bdg/common/server/text_viewer_panel.hpp"

#include "modules/bdg/common/server/tool_form.hpp"
#include "modules/bdg/common/server/ui_helpers.hpp"

#include <context/file_service.hpp>

#include <fstream>
#include <system_error>

namespace bdg::wish::common {

using namespace bison;

namespace {

// Owner of the panel's private directory (see file_service::app_private_dir()).
constexpr const char* kTextViewerApp = "bdg/common/text_viewer";

constexpr const char* kTextViewerLayout = R"json({
  "type": "Window", "title": "Details", "width": 820, "height": 420,
  "closable": true,
  "children": { "vbox": { "type": "VerticalLayout", "spacing": 4, "children": {
    "toolbar": { "type": "HorizontalLayout", "spacing": 8, "children": {
      "target":      { "type": "Label", "text": "(nothing selected)" },
      "spring":      { "type": "Spring" },
      "btn_refresh": { "type": "Button", "label": "Refresh", "icon": "res/icons/refresh.png" }
    } },
    "sep": { "type": "Separator" },
    "editor": {
      "type": "TextEditor", "file_path": "", "language": "none", "read_only": true,
      "auto_scroll": false, "width": -1, "height": -1
    }
  } } }
})json";

} // namespace

void text_viewer_panel::build(
    tool_form& owner,
    const std::string& root_key,
    text_viewer_options options,
    const std::function<void(ui_tree&)>& wire) {
  root_key_ = root_key;
  // Form root keys are unique only within one session, so the files go in a
  // temp directory owned by this session: sessions sharing a persistent
  // sandbox cannot overwrite each other's, and the server removes it at
  // session end. The "private/" prefix keeps them out of the browser cache.
  auto& sess = owner.sess();
  std::string dir = sess.file_service ? sess.file_service->create_temp_dir(kTextViewerApp) : std::string{"private"};
  file_prefix_ = dir + "/" + owner.internal_root_key_ + "_" + options.file_stem + "_";
  // Captured now: remove_file() runs on close, outside dispatch, where sess()
  // is unavailable; resource_dir is fixed for the session.
  resource_dir_ = sess.resource_dir;

  const bool custom_layout = options.layout_json != nullptr;
  owner.build_window(
      root_key_, custom_layout ? options.layout_json : kTextViewerLayout, window_id_, [&](ui_tree& tree) {
        tree.with("vbox.toolbar.target", [&](const auto& e) { target_label_ = e; });
        tree.with("vbox.editor", [&](const auto& e) { editor_ = e; });
        tree.with("vbox.toolbar.btn_refresh", [&](const auto& e) {
          if (options.on_refresh)
            owner.on_click(wish_id_of(e), options.on_refresh);
        });
        if (!custom_layout) {
          tree.with("", [&](const auto& e) {
            e["title"_key] = options.title;
            e["width"_key] = options.width;
            e["height"_key] = options.height;
          });
          if (target_label_)
            target_label_["text"_key] = options.placeholder;
          if (editor_) {
            editor_["language"_key] = options.language;
            editor_["auto_scroll"_key] = options.auto_scroll;
          }
        }
        if (wire)
          wire(tree);
      });
}

void text_viewer_panel::set_title(const std::string& title) {
  if (target_label_)
    target_label_["text"_key] = title;
}

void text_viewer_panel::set_text(const std::string& text) {
  if (!editor_)
    return;
  std::string rel = file_prefix_ + std::to_string(next_seq_++) + ".txt";
  auto path = file_service::resolve_path(rel, resource_dir_, /*allow_absolute=*/false);
  if (path.empty())
    return;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  {
    std::ofstream out(path, std::ios::binary);
    if (!out)
      return;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  remove_file();
  file_ = rel;
  editor_["file_path"_key] = rel;
}

void text_viewer_panel::remove_file() {
  if (file_.empty())
    return;
  std::error_code ec;
  std::filesystem::remove(resource_dir_ / file_, ec);
  file_.clear();
}

} // namespace bdg::wish::common
