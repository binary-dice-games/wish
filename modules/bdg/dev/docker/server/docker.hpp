// MIT License © 2026 Binary Dice Games
/// @file docker.hpp
/// @brief Server-side DockerFrontend form -- a Docker Desktop-style GUI over
///        the local `docker` CLI.
///
/// All `docker` invocation and output parsing happens client-side (see
/// client/docker_source.hpp) -- the Docker daemon whose state matters is the
/// one reachable from the user's own machine. This form only renders whatever
/// snapshot it was last given via its update_* RMI methods, and emits
/// high-level *_requested events the client reacts to by running the
/// corresponding `docker` command and pushing a fresh snapshot. Mirrors the
/// `git` module's client/server split (server/git.hpp).
///
/// Destructive actions (stop / kill / remove / prune) are gated behind the
/// built-in MessageBox form (form::instantiate_child_form(), "yes_no"
/// preset) -- see show_confirm() below, a direct port of
/// git_repo::show_confirm().
///
/// Owns four independently dockable Windows -- Containers (the main root),
/// Images, Volumes, Networks -- registered by hand in on_init() exactly as
/// git.cpp's build_*_window() does. The Logs / Inspect windows follow in a
/// later step (see PLAN.md).
#pragma once

#include <ui/forms/form.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class message_box;

/// @brief Docker Desktop-style GUI form for the local `docker` CLI.
///
/// Emitted events (see DESIGN.md "5. Public API Contract"):
///   - `"closed"` -- the main (Containers) window was closed.
///   - `"refresh_requested"` -- no payload; any window's Refresh button.
///   - `"container_action_requested"` -- `{ id, action }` where `action` is
///     one of `start`, `stop`, `restart`, `pause`, `unpause`, `kill`,
///     `remove`.
///   - `"image_action_requested"` -- `{ id, action }` (`run`, `remove`).
///   - `"volume_action_requested"` -- `{ name, action }` (`remove`).
///   - `"network_action_requested"` -- `{ id, action }` (`remove`).
///   - `"prune_requested"` -- `{ scope }` (`containers`, `images`,
///     `volumes`, `networks`).
///   - `"pull_image_requested"` -- `{ ref }`.
///   - `"create_volume_requested"` -- `{ name }`.
///   - `"logs_requested"` -- `{ id, follow, lines }`.
///   - `"inspect_requested"` -- `{ kind, id }`.
class docker_frontend : public form {
 public:
  explicit docker_frontend(bison::dynamic&& base);

  /// @brief RMI method: replace the Containers table. @p args.containers is
  /// a dynamic array, each `{ id, name, image, state
  /// ("running"/"exited"/"paused"/"created"/"restarting"/"dead"/...), status
  /// (human string), ports (string), created (string) }`.
  bison::dynamic do_update_containers(const bison::dynamic& args);

  /// @brief RMI method: replace the Images table. @p args.images is a
  /// dynamic array, each `{ id, repository, tag, created, size }`.
  bison::dynamic do_update_images(const bison::dynamic& args);

  /// @brief RMI method: replace the Volumes table. @p args.volumes is a
  /// dynamic array, each `{ name, driver, mountpoint }`.
  bison::dynamic do_update_volumes(const bison::dynamic& args);

  /// @brief RMI method: replace the Networks table. @p args.networks is a
  /// dynamic array, each `{ id, name, driver, scope }`.
  bison::dynamic do_update_networks(const bison::dynamic& args);

  /// @brief RMI method: fill the Logs window's text. @p args holds
  /// `container_id` (echoed from `logs_requested` -- the call is discarded
  /// if it no longer matches the window's open target, git's do_update_diff
  /// staleness guard), `title` (string) and `text` (string; split on `\n`
  /// into one row per line).
  bison::dynamic do_update_logs(const bison::dynamic& args);

  /// @brief RMI method: fill the Inspect window's text. @p args holds
  /// `target_id` (staleness guard), `kind`, `title` and `text`.
  bison::dynamic do_update_inspect(const bison::dynamic& args);

  /// @brief RMI method: report the result of a client-run `docker` command,
  /// shown in the relevant window's status label. @p args holds `command`
  /// (string), `ok` (bool), `output` (string, shown on failure), and an
  /// optional `scope` ("containers"/"images"/"volumes"/"networks", default
  /// "containers") selecting which window's status label to write.
  bison::dynamic do_command_result(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  // ── Generic list-window plumbing ──────────────────────────────────────
  //
  // Containers / Images / Volumes / Networks are four near-identical
  // toolbar + Table windows. One `list_window` bundles the per-window
  // widgets; one `list_row` type + one dispatch map (`menu_action_targets_`
  // keyed by scope) serve all four.

  struct list_window {
    std::string root_key;
    bison::key_t window_id;
    ui_element_ptr status_label;
    ui_element_ptr table;
  };

  struct list_row {
    ui_element_ptr row;
    std::string scope; // "container" / "image" / "volume" / "network"
    std::string key;   // container/image/network id, or volume name
    std::string name;  // display name (filter + confirm message)
    std::string extra; // container: image ref (for the text filter)
    std::string state; // container only
    size_t child_key{0};
    std::vector<bison::key_t> object_ids; // erased together on rebuild
  };

  struct row_action {
    std::string scope;
    std::string key;
    std::string action;
  };

  /// @brief Import @p layout_json, register every node, cache the toolbar /
  /// status / table widgets, and register the tree as its own dockable
  /// top-level root at @p lw.root_key (git.cpp's build_*_window() pattern).
  /// @p wire_toolbar is called with the imported `ui_tree` to bind that
  /// window's own toolbar buttons / inline fields.
  void build_list_window(
      list_window& lw, const char* layout_json, const std::string& root_key,
      const std::function<void(ui_tree&)>& wire_toolbar);

  /// @brief Clear every dynamically-added TableRow from @p lw.table (and its
  /// cells' ctx().objects entries), plus the matching entries in
  /// @p rows / menu_action_targets_.
  void clear_list_rows(list_window& lw, std::vector<list_row>& rows, size_t& next_key);

  /// @brief Append one row: @p cells (already-built Label cells, left to
  /// right) + a `...` MenuButton built from @p items ({label, action,
  /// confirm} triples; an empty label inserts a Separator). Records the row
  /// in @p rows and wires each MenuItem into menu_action_targets_.
  struct menu_spec {
    std::string label;
    std::string action;
    bool confirm{false};
  };
  void add_list_row(
      list_window& lw, std::vector<list_row>& rows, size_t& next_key, list_row&& meta,
      const std::vector<ui_element_ptr>& cells, const std::vector<menu_spec>& items);

  void set_status(list_window& lw, const std::string& text, bool ok);

  // ── Per-window rebuild ───────────────────────────────────────────────
  void rebuild_containers(const bison::dynamic& args);
  void rebuild_images(const bison::dynamic& args);
  void rebuild_volumes(const bison::dynamic& args);
  void rebuild_networks(const bison::dynamic& args);

  // ── Logs / Inspect text panes ────────────────────────────────────────
  /// @brief Build a Logs- or Inspect-style window: a toolbar + a
  /// single-column scrolling `Table` of `Label` lines. @p wire_toolbar
  /// binds that window's own toolbar controls.
  void build_text_window(
      const std::string& root_key, const char* layout_json, bison::key_t& window_id_out,
      ui_element_ptr& table_out, const std::function<void(ui_tree&)>& wire_toolbar);
  /// @brief Clear @p table's rows (+ their ctx().objects entries recorded in
  /// @p line_ids) and repopulate one `Label` row per line of @p text.
  void set_text_lines(
      const ui_element_ptr& table, std::vector<bison::key_t>& line_ids, size_t& next_key, const std::string& text);
  void emit_logs_request();
  void emit_inspect_request();

  /// @brief Re-apply the Containers text filter + state Combo to each
  /// container row's `visible` field (git's retroactive-filter pattern).
  void apply_container_filter();

  // ── Confirmation modal (git_repo::show_confirm() port) ────────────────
  void show_confirm(const std::string& message, std::function<void()> on_confirm);

  // ── Small builders ───────────────────────────────────────────────────
  void assign_id(const ui_element_ptr& el);
  void set_children_list(const ui_element_ptr& parent, const std::vector<ui_element_ptr>& kids);
  ui_element_ptr make_label(const std::string& text, const char* light = nullptr, const char* dark = nullptr);

  // ── State ────────────────────────────────────────────────────────────
  std::string title_;

  list_window containers_;
  list_window images_;
  list_window volumes_;
  list_window networks_;

  std::vector<list_row> container_rows_;
  std::vector<list_row> image_rows_;
  std::vector<list_row> volume_rows_;
  std::vector<list_row> network_rows_;
  size_t next_container_key_{0};
  size_t next_image_key_{0};
  size_t next_volume_key_{0};
  size_t next_network_key_{0};

  // Containers-window filter state.
  ui_element_ptr filter_input_;
  bison::key_t filter_input_id_;
  bison::key_t state_combo_id_;
  std::string filter_text_;
  int32_t state_filter_{0}; // 0 = All, 1 = Running, 2 = Stopped

  // Inline toolbar fields (Images: pull ref; Volumes: new name).
  ui_element_ptr pull_ref_input_;
  bison::key_t pull_ref_input_id_;
  std::string pull_ref_text_;
  ui_element_ptr volume_name_input_;
  bison::key_t volume_name_input_id_;
  std::string volume_name_text_;

  std::shared_ptr<message_box> confirm_dialog_;

  // Logs window.
  std::string logs_root_key_;
  bison::key_t logs_window_id_;
  ui_element_ptr logs_table_;
  ui_element_ptr logs_target_label_;
  bison::key_t logs_follow_id_;
  bison::key_t logs_lines_id_;
  std::vector<bison::key_t> logs_line_ids_;
  size_t next_logs_line_key_{0};
  std::string open_logs_id_;   // container id whose logs the window shows
  std::string open_logs_name_; // its display name (for the target label)
  bool logs_follow_{false};
  int32_t logs_lines_{500};

  // Inspect window.
  std::string inspect_root_key_;
  bison::key_t inspect_window_id_;
  ui_element_ptr inspect_table_;
  ui_element_ptr inspect_target_label_;
  std::vector<bison::key_t> inspect_line_ids_;
  size_t next_inspect_line_key_{0};
  std::string open_inspect_id_;
  std::string open_inspect_kind_;
  std::string open_inspect_name_;

  std::unordered_map<bison::key_t, std::function<void()>, bison::key_t, bison::key_t> click_handlers_;
  std::unordered_map<bison::key_t, row_action, bison::key_t, bison::key_t> menu_action_targets_;
};

/// @brief Register DockerFrontend in the "wish" bison namespace.
void register_docker();

} // namespace bdg::wish
