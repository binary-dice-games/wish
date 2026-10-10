// MIT License © 2026 Binary Dice Games
/// @file nymph.hpp
/// @brief Server-side Nymph form -- turns a text description of a chart
///        into a PNG drawn with wish's plot widgets, and edits one live.
///
/// See DESIGN.md. The form never touches a local file: the client uploads
/// the input (a source text, or a nymph PNG carrying one) into the session
/// sandbox, and downloads the PNG the form writes there.
///
/// Three modes, chosen at construction (`{silent: bool, view: bool}`):
/// - **silent**: no visible UI. `load` validates the input (throwing on any
///   error); `render` queues a render whose result arrives as the
///   `"rendered"` / `"render_failed"` event.
/// - **view**: the Preview and Data windows only, read-only. There is no
///   source editor and no Save; `load` throws for a bad source as in silent
///   mode. Closing the Preview window emits `"closed"`.
/// - **edit** (default): three dockable windows -- Source (Save button, file
///   name, error banner, and one TextEditor per part), Preview (the bound
///   plot as live widgets) and Data (a Table of the CSV). Save and Ctrl+S
///   render through the same code as silent mode and emit `"on_image_saved"`.
///
/// Every render runs in on_event(), on the server's render thread: `render`
/// reaches it by queueing a self-addressed event (DESIGN.md "5. Data Flow").
///
/// RMI methods: `load({path, display_path?, validate?})`, `render({})`,
/// `source({})` -> `{path}`, `mark_saved({})`.
/// Events: `"rendered" {path}`, `"render_failed" {message}`,
/// `"on_image_saved" {path}`, `"closed"`.
#pragma once

#include "nymph_bind.hpp"
#include "nymph_document.hpp"

#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/forms/message_box.hpp>
#include <ui/ui_element.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace bdg::wish {

/// @brief The `Nymph` form. See the file comment for its contract.
class nymph_form : public common::tool_form {
 public:
  explicit nymph_form(bison::dynamic&& base);

  /// @brief `__construct`: reads `{silent: bool, view: bool}`.
  void on_construct(const bison::dynamic& params);
  /// @brief `load`: replace the current document with a sandbox file.
  /// @throws std::runtime_error in silent and view mode for any problem
  ///         with the input; in edit mode only for an unusable `path`.
  bison::dynamic do_load(const bison::dynamic& args);
  /// @brief `render`: queue a render of the current document.
  /// @throws std::runtime_error if nothing is loaded.
  bison::dynamic do_render(const bison::dynamic& args);
  /// @brief `source`: write the current source text to a sandbox file.
  /// @return `{path}`. @throws std::runtime_error if nothing is loaded.
  bison::dynamic do_source(const bison::dynamic& args);
  /// @brief `mark_saved`: the client stored the last saved image locally.
  bison::dynamic do_mark_saved(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t id, bison::key_t event, const bison::dynamic& payload) override;

 private:
  /// Runs @p fn with the session, whether or not the caller is inside RMI
  /// dispatch (on_event() is not; the RMI methods are).
  void with_session(const std::function<void(context&)>& fn);

  std::string read_sandbox_file(const std::string& name);
  void write_sandbox_file(const std::string& name, const std::string& bytes);

  /// The current source text: the loaded text in silent mode, the three
  /// editors' contents joined in edit mode.
  std::string current_source();
  /// The current document, re-read from the editors' files in edit mode.
  nymph::document current_document();

  /// Renders the current document to `nymph_out_<n>.png` in the sandbox and
  /// emits @p done_event `{path}`, or `"render_failed" {message}`.
  /// @return false if nothing was written.
  bool render_to_sandbox(bison::key_t done_event);

  /// Silent and view mode keep the loaded text and its bound figure; edit
  /// mode reads its three editors instead.
  bool text_backed() const { return silent_ || view_; }

  // ── view mode ──
  /// Builds (or, on a reload, rebuilds) the read-only Preview and Data windows.
  void show_view();

  // ── edit mode ──
  void build_edit_ui();
  /// Points the three editors at fresh sandbox files holding @p doc.
  void show_document(const nymph::document& doc);
  /// Binds the editors' current text; updates banner, preview and data.
  void rebind();
  void rebuild_preview(nymph::figure& fig);
  void rebuild_data(const nymph::table& data);
  /// Registers @p root (a descriptor) as the top-level window @p root_key,
  /// reusing @p window_id so ImGui keeps the window's position and dock.
  void replace_window(const std::string& root_key, bison::key_t& window_id, const bison::dynamic& root);
  void clear_window(const std::string& root_key);
  void set_banner(const std::string& text);
  void update_path_label();
  void show_close_confirm();
  void request_close();

  bool silent_{false};
  bool view_{false};
  bool view_built_{false};                ///< View mode: its two windows exist.
  bool loaded_{false};
  nymph::document document_;              ///< As loaded; edit mode keeps only its separators current.
  std::string source_text_;               ///< Silent mode: the loaded source, verbatim.
  std::optional<nymph::figure> figure_;   ///< Silent mode: bound at load.
  std::string display_path_;
  int file_counter_{0};                   ///< Makes every sandbox file name fresh.

  bison::key_t holder_id_;                ///< The invisible root's id; target of the self-addressed event.

  // ── edit mode state ──
  bool ui_built_{false};
  bool dirty_{false};
  bool bind_ok_{false};
  bool pending_close_after_save_{false};
  std::string source_root_key_;
  std::string preview_root_key_;
  std::string data_root_key_;
  bison::key_t source_window_id_;
  bison::key_t preview_window_id_;
  bison::key_t data_window_id_;
  bison::key_t save_button_id_;
  bison::key_t description_editor_id_;
  bison::key_t format_editor_id_;
  bison::key_t data_editor_id_;
  ui_element_ptr description_editor_;
  ui_element_ptr format_editor_;
  ui_element_ptr data_editor_;
  ui_element_ptr banner_;
  ui_element_ptr path_label_;
  std::string description_file_;
  std::string format_file_;
  std::string data_file_;
  std::string shown_data_;                ///< CSV text the Data window currently shows.
  bool data_shown_{false};
  std::shared_ptr<message_box> close_dialog_;
};

/// @brief Register the `Nymph` class in the "wish" bison namespace.
void register_nymph();

} // namespace bdg::wish
