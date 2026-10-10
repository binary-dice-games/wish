// MIT License © 2026 Binary Dice Games
/// @file text_viewer_panel.hpp
/// @brief A read-only text window (Logs / Describe / Inspect / Details)
///        shared by the bdg/dev tool forms.
///
/// The window is a toolbar -- a `target` Label naming what is shown, a
/// Spring and a Refresh button -- over a read-only `TextEditor`. A
/// TextEditor displays a file, so set_text() writes each update into the
/// session sandbox as `<temp dir>/<form root>_<stem>_<n>.txt` (resolved via
/// file_service::resolve_path()) and deletes the file it replaces. The temp
/// dir comes from file_service::create_temp_dir(): owned by this session
/// and removed at its end, and under "private/" because tool output can
/// carry secrets. A fresh name every call: the renderer only reloads when
/// `file_path` changes.
///
/// The module keeps the identity of what the window shows (a pod, a
/// release, ...) and drops responses for anything else -- the staleness
/// guard of each `update_<viewer>` RMI method.
#pragma once

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_object.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace bdg::wish::common {

class tool_form;

/// @brief Settings for text_viewer_panel::build().
struct text_viewer_options {
  std::string title{"Details"};
  /// TextEditor highlighting language (`"yaml"`, `"json"`, `"log"`,
  /// `"none"`, ...).
  std::string language{"none"};
  /// The target label's text before anything is shown.
  std::string placeholder{"(nothing selected)"};
  int32_t width{820};
  int32_t height{420};
  /// Keep the editor scrolled to the last line (a log being followed).
  bool auto_scroll{false};
  /// Distinguishes this window's sandbox files from the form's other
  /// viewers, e.g. `"logs"`.
  std::string file_stem{"details"};
  /// Run by the toolbar's Refresh button.
  std::function<void()> on_refresh;
  /// A layout to use instead of the default one -- for a toolbar with extra
  /// controls. Must keep `vbox.toolbar.target`, `vbox.toolbar.btn_refresh`
  /// and `vbox.editor`; title / language / placeholder / size are then
  /// taken from the layout, not from these options.
  const char* layout_json{nullptr};
};

class text_viewer_panel {
 public:
  /// @brief Builds the window as @p owner's top-level root @p root_key and
  /// caches the session sandbox directory. Call from the owner's on_init()
  /// (inside RMI dispatch). @p wire binds any extra toolbar controls.
  void build(
      tool_form& owner,
      const std::string& root_key,
      text_viewer_options options = {},
      const std::function<void(ui_tree&)>& wire = {});

  /// @brief Sets the target label's text.
  void set_title(const std::string& title);

  /// @brief Shows @p text in the editor (see the file comment). A write
  /// failure leaves the editor unchanged.
  void set_text(const std::string& text);

  /// @brief Deletes the sandbox file the editor shows, if any. Call when the
  /// form closes; safe outside RMI dispatch.
  void remove_file();

  bison::key_t window_id() const {
    return window_id_;
  }
  const std::string& root_key() const {
    return root_key_;
  }
  const ui_element_ptr& editor() const {
    return editor_;
  }
  /// The sandbox-relative file the editor shows; empty before set_text().
  const std::string& file() const {
    return file_;
  }

 private:
  std::string root_key_;
  std::string file_prefix_; // "<session temp dir>/<form root>_<stem>_"
  bison::key_t window_id_;
  ui_element_ptr target_label_;
  ui_element_ptr editor_;
  std::filesystem::path resource_dir_;
  std::string file_;
  size_t next_seq_{0};
};

} // namespace bdg::wish::common
