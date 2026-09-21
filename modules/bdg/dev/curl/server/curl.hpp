// MIT License © 2026 Binary Dice Games
/// @file curl.hpp
/// @brief Server-side CurlFrontend form -- a Postman-style GUI over the
///        local `curl` binary, for building and sending REST API requests.
///
/// All `curl` invocation and output parsing happens client-side (see
/// client/curl_source.hpp) -- mirrors the `docker`/`kubectl` modules'
/// client/server split. This form only renders whatever snapshot it was
/// last given via its update_* RMI methods, and emits high-level
/// *_requested events the client reacts to.
///
/// Unlike docker/kubectl, which only ever populate *read-only* display
/// tables, this form also owns a small number of *editable* repeating
/// key/value row lists (Params, Headers, Form-body, Environment
/// variables) -- see the `kv_table`/`kv_row` plumbing below. Their current
/// contents are read directly from the live `InputText`/`Checkbox`
/// widgets at Send/Save time rather than tracked in a separate C++
/// mirror, so typing in one row is never disturbed by an unrelated
/// refresh (see DESIGN.md "Editable key-value tables").
///
/// Owns six independently dockable Windows -- Request (the main root),
/// Response, History, Collections, Environments, and Console (a
/// FIFO-capped trace of every `curl` invocation the client ran, fed by
/// append_command_log -- the docker/kubectl "Console" window) --
/// registered by hand in on_init() exactly as docker.cpp's
/// build_list_window() / build_text_window() do.
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class message_box;

/// @brief Postman-style GUI form for the local `curl` binary.
///
/// Emitted events (see DESIGN.md "5. Public API Contract"):
///   - `"closed"` -- the main (Request) window was closed.
///   - `"send_requested"` -- the current Request builder state, snapshotted
///     at the moment "Send" was clicked.
///   - `"save_request_requested"` -- the current builder state plus a
///     target collection/name, from the inline "Save as" fields.
///   - `"load_request_requested"` / `"delete_request_requested"` /
///     `"duplicate_request_requested"` -- `{ id }`, a Collections row action.
///   - `"load_history_requested"` -- `{ id }`, a History row action.
///   - `"clear_history_requested"` -- no payload.
///   - `"new_environment_requested"` -- `{ name }`.
///   - `"delete_environment_requested"` -- `{ id }`.
///   - `"select_environment_requested"` -- `{ id }` (empty id = deselect).
///   - `"save_environment_vars_requested"` -- `{ id, vars: [{key,value}] }`.
class curl_frontend : public form {
 public:
  explicit curl_frontend(bison::dynamic&& base);

  /// @brief RMI method: render the Response window from one finished
  /// request. @p args holds `ok` (bool), `error` (string, set on a
  /// connection-level failure -- everything else is then blank),
  /// `status_code` (int32), `status_text` (string), `time_ms` (float),
  /// `size_bytes` (float), `headers` (dynamic array of `{key, value}`),
  /// `body_text` (string, verbatim -- used only for the Headers-tab-less
  /// plain fallback / Console preview), `body_file` (string, a
  /// client-uploaded session-sandbox path the Body TextEditor points at;
  /// empty when the body was binary/too large), `body_is_json` (bool).
  bison::dynamic do_update_response(const bison::dynamic& args);

  /// @brief RMI method: pre-fill the Request builder from a stored
  /// History/Collections entry. @p args holds `method`, `url`, `params`/
  /// `headers`/`form_fields` (each a dynamic array of `{key, value,
  /// enabled}`), `body_mode` (`"none"`/`"raw"`/`"json"`/`"form"`),
  /// `body_text`, `auth_mode` (`"none"`/`"basic"`/`"bearer"`),
  /// `auth_username`, `auth_password`, `auth_token`.
  bison::dynamic do_update_request_builder(const bison::dynamic& args);

  /// @brief RMI method: replace the History table. @p args.entries -- each
  /// `{ id, method, url, status_code (int32), ok (bool), time_ms (float),
  /// timestamp (string) }`, newest first.
  bison::dynamic do_update_history(const bison::dynamic& args);

  /// @brief RMI method: replace the Collections table. @p args.entries --
  /// each `{ id, collection, name, method, url }`.
  bison::dynamic do_update_collections(const bison::dynamic& args);

  /// @brief RMI method: replace the Environments summary table.
  /// @p args.entries -- each `{ id, name, var_count (int32) }`.
  bison::dynamic do_update_environments(const bison::dynamic& args);

  /// @brief RMI method: populate the Environments window's variable editor
  /// for the environment the user just selected. @p args holds
  /// `environment_id`, `name`, `vars` (dynamic array of `{key, value}`).
  /// An empty `environment_id` clears the editor (nothing selected).
  bison::dynamic do_update_environment_vars(const bison::dynamic& args);

  /// @brief RMI method: append one row to the Console window's `curl`
  /// subprocess trace. @p args holds `command` (string), `exit_code`
  /// (int32), `ok` (bool) and `output` (string, single-line preview).
  /// Mirrors docker_frontend::do_append_command_log.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Editable key/value row lists (Params, Headers, Form body, Env vars) ──
  //
  // Unlike docker/kubectl's read-only list_window/list_row, these rows hold
  // live-editable InputText/Checkbox widgets. Reading the current entries
  // means reading each row's widgets' own "value" field directly -- never
  // a separate C++-side mirror -- so a background refresh (there is none
  // here) could never clobber what the user is mid-typing. Adding a row
  // appends one row; removing erases just that one row's objects -- a
  // kv_table is never fully cleared-and-rebuilt except when explicitly
  // loading stored data over it (kv_table_load()).

  struct kv_row {
    ui_element_ptr row;
    ui_element_ptr enabled_box; // null when has_enabled == false
    ui_element_ptr key_input;
    ui_element_ptr value_input;
    bison::key_t remove_button_id;
    size_t child_key{0};
    std::vector<bison::key_t> object_ids;
  };

  struct kv_table {
    ui_element_ptr table;
    bool has_enabled{true};
    std::vector<kv_row> rows;
    size_t next_key{0};
  };

  /// @brief Append one row (Checkbox? + InputText key + InputText value +
  /// a small "x" remove Button) to @p kv, seeded with @p key/@p value/
  /// @p enabled.
  void kv_table_add_row(
      kv_table& kv, const std::string& key = "", const std::string& value = "", bool enabled = true);
  /// @brief Erase exactly one row (by its remove Button's widget id).
  void kv_table_remove_row(kv_table& kv, bison::key_t remove_button_id);
  /// @brief Erase every row's objects and reset the table to empty.
  void kv_table_clear(kv_table& kv);
  /// @brief kv_table_clear() then kv_table_add_row() once per element of
  /// @p args[@p field_key] (`{key, value}` plus `enabled` when
  /// @p kv.has_enabled).
  void kv_table_load(kv_table& kv, const bison::dynamic& args, bison::key_t field_key);
  /// @brief Read the live widget values back out as a dynamic array of
  /// `{key, value, enabled}` (or `{key, value}` when !has_enabled),
  /// wrapped as a `dynamic_ptr` ready to assign to a field (`field`'s
  /// variant has no bare-`dynamic` alternative -- see `bison_object.hpp`).
  bison::dynamic_ptr kv_table_read(const kv_table& kv) const;

  // ── Generic read-only list-window plumbing (History / Collections) ──────
  // A slimmed-down version of docker's list_window/list_row: one row menu
  // action, no per-scope colour coding.

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;
  };

  struct list_row {
    ui_element_ptr row;
    std::string id; // opaque id the client assigned this entry
    size_t child_key{0};
    std::vector<bison::key_t> object_ids;
  };

  struct row_action {
    std::string scope; // "history" / "collections"
    std::string id;
    std::string action;
  };

  void build_list_window(
      list_window& lw, const char* layout_json, const std::string& root_key,
      const std::function<void(ui_tree&)>& wire_toolbar);
  void clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key);
  struct menu_spec {
    std::string label;
    std::string action;
  };
  void add_list_row(
      list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
      const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items, const std::string& scope);
  void set_status(list_window& lw, const std::string& text, bool ok);

  /// @brief Import @p layout_json, assign every node a wish RMI id, call
  /// @p wire (before the tree is moved away) to cache widget pointers /
  /// bind handlers, then register the tree as its own dockable top-level
  /// root at @p root_key. docker.cpp's build_text_window()/
  /// build_stats_window() pattern, generalized for any layout -- used for
  /// every window except the main Request window (registered by hand in
  /// on_init(), since form::init() already registers internal_root_key_).
  void build_window(
      const char* layout_json, const std::string& root_key, bison::key_t& window_id_out,
      const std::function<void(ui_tree&)>& wire);

  // ── Small builders (docker.cpp's shape) ──────────────────────────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  void show_confirm(const std::string& message, std::function<void()> on_confirm);

  // ── Request builder: collecting current state into an event payload ────
  bison::dynamic collect_request_state() const;
  void set_status_line(const std::string& text, const char* light, const char* dark);
  /// @brief Show the Raw/JSON text box or the Form key-value editor
  /// depending on @p idx (the Body mode Combo's index: 0 none / 1 raw /
  /// 2 json / 3 form). Takes the index explicitly rather than re-reading
  /// body_mode_combo_'s own field -- the "changed" handler must act on
  /// the value in the event payload (docker's established convention:
  /// see e.g. its state_combo_id_ handler), since nothing guarantees the
  /// widget's own field has already been updated to match by the time
  /// on_event runs; do_update_request_builder() already has the index it
  /// just wrote, so both call sites simply pass what they know.
  void apply_body_mode_visibility(int32_t idx);
  /// @brief Same idea as apply_body_mode_visibility() for the Auth tab's
  /// Basic-Auth vs. Bearer-Token field groups (@p idx: 0 none / 1 basic /
  /// 2 bearer).
  void apply_auth_mode_visibility(int32_t idx);
  void rebuild_response_headers(const bison::dynamic& args);

  // ── State ────────────────────────────────────────────────────────────
  std::string title_;

  // Request window.
  bison::key_t request_window_id_;
  ui_element_ptr method_combo_;
  bison::key_t method_combo_id_;
  ui_element_ptr url_input_;
  bison::key_t url_input_id_;
  ui_element_ptr env_combo_;
  bison::key_t env_combo_id_;
  bison::key_t follow_redirects_id_;
  bool follow_redirects_{true};
  ui_element_ptr status_line_label_;

  kv_table params_;
  kv_table headers_;
  kv_table body_form_;
  ui_element_ptr body_mode_combo_;
  bison::key_t body_mode_combo_id_;
  ui_element_ptr body_raw_box_;  // wraps body_text_input_; visible for Raw/JSON
  ui_element_ptr body_form_box_; // wraps body_form_'s toolbar+table; visible for Form
  ui_element_ptr body_text_input_;
  ui_element_ptr auth_mode_combo_;
  bison::key_t auth_mode_combo_id_;
  ui_element_ptr auth_basic_box_;  // wraps username/password; visible for Basic
  ui_element_ptr auth_bearer_box_; // wraps token; visible for Bearer
  ui_element_ptr auth_username_input_;
  ui_element_ptr auth_password_input_;
  ui_element_ptr auth_token_input_;

  ui_element_ptr save_name_input_;
  ui_element_ptr save_collection_input_;
  std::string save_name_text_;
  std::string save_collection_text_;

  // Environment names for the Request window's env_combo_ (index -> id).
  std::vector<std::string> environment_ids_;

  // Response window.
  std::string response_root_key_;
  bison::key_t response_window_id_;
  ui_element_ptr response_status_label_;
  ui_element_ptr response_headers_table_;
  ui_element_ptr response_body_editor_; // TextEditor, file_path set per response
  std::vector<bison::key_t> response_header_row_ids_;
  size_t next_response_header_key_{0};

  // History window.
  std::string history_root_key_;
  list_window history_;
  std::vector<list_row> history_rows_;
  size_t next_history_key_{0};

  // Collections window.
  std::string collections_root_key_;
  list_window collections_;
  std::vector<list_row> collection_rows_;
  size_t next_collection_key_{0};

  // Environments window.
  std::string environments_root_key_;
  list_window environments_;
  std::vector<list_row> environment_rows_;
  size_t next_environment_key_{0};
  ui_element_ptr env_new_name_input_;
  std::string env_new_name_text_;
  ui_element_ptr env_editor_label_;
  kv_table env_vars_;
  std::string open_environment_id_;

  // Console window (client `curl` subprocess trace) -- docker's shape.
  std::string console_root_key_;
  bison::key_t console_window_id_;
  ui_element_ptr console_table_;
  static constexpr size_t kMaxConsoleRows = 500;
  struct console_row_entry {
    size_t child_key;
    std::vector<bison::key_t> object_ids;
  };
  size_t console_seq_{0};
  size_t next_console_child_key_{0};
  std::deque<console_row_entry> console_rows_;
  void append_console_row(const std::string& command, int32_t exit_code, bool ok, const std::string& output);
  void clear_console_rows();
  void erase_console_row_objects(const console_row_entry& entry);

  std::shared_ptr<message_box> confirm_dialog_;

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
  std::unordered_map<bison::key_t, row_action, bison::key_t, bison::key_t> menu_action_targets_;
  // Remove-row-button widget id -> which kv_table it belongs to.
  std::unordered_map<bison::key_t, kv_table*, bison::key_t, bison::key_t> kv_remove_targets_;
};

/// @brief Register CurlFrontend in the "wish" bison namespace.
void register_curl();

} // namespace bdg::wish
