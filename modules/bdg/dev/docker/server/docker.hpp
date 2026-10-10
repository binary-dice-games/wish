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
/// Built on the panels shared by the bdg tool forms
/// (modules/bdg/common/server): destructive actions (stop / kill / remove /
/// prune) are gated behind tool_form::show_confirm(); Containers (the main
/// root), Images, Volumes and Networks are common::list_panel; Logs and
/// Inspect are common::text_viewer_panel; Console (a trace of every
/// `docker` command the client ran) is common::console_panel; the Stats
/// window's graphs are common::rolling_plot. Every window docks
/// independently.
#pragma once

#include "modules/bdg/common/server/console_panel.hpp"
#include "modules/bdg/common/server/list_panel.hpp"
#include "modules/bdg/common/server/rolling_plot.hpp"
#include "modules/bdg/common/server/text_viewer_panel.hpp"
#include "modules/bdg/common/server/tool_form.hpp"

#include <ui/ui_element.hpp>
#include <ui/ui_importer.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace bdg::wish {

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
class docker_frontend : public common::tool_form {
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

  /// @brief RMI method: append one row to the Console window's `docker`
  /// subprocess trace. @p args holds `command` (string, e.g. `"docker ps
  /// -a"`), `exit_code` (int32), `ok` (bool) and `output` (string, a
  /// single-line preview). See common::console_panel.
  bison::dynamic do_append_command_log(const bison::dynamic& args);

  /// @brief RMI method: push one live `docker stats` sample to the Stats
  /// window. @p args holds `entries` -- a dynamic array, each `{ name,
  /// cpu_percent (float), mem_percent (float), mem_usage (string) }` -- and
  /// an optional `error` (string; shown in the status label when the client
  /// could not run `docker stats`). Each call appends one point to every
  /// per-container CPU%/Mem% line plus the aggregate "Total" line, rebuilds
  /// the current-values table, and adds/removes series as containers come
  /// and go. Fed by docker_source's background poll thread -- deliberately
  /// NOT traced in the Console window (a 3 s re-poll would flood it, the
  /// same reason the Logs "Follow" thread bypasses it).
  bison::dynamic do_update_stats(const bison::dynamic& args);

 protected:
  void on_init() override;
  void on_event(bison::key_t widget_id, bison::key_t event_name, const bison::dynamic& payload) override;

 private:
  /// One row of a list window: what the filter and row actions work on.
  struct entity {
    std::string scope; // "container" / "image" / "volume" / "network"
    std::string key;   // container/image/network id, or volume name
    std::string name;  // display name (filter + confirm message)
    std::string extra; // container: image ref (for the text filter)
    std::string state; // container only
  };
  using list_window = common::list_panel<entity>;

  /// @brief Builds @p lw from @p layout_json at @p root_key, binding its
  /// Refresh button and a Prune button confirming @p prune_message and
  /// emitting `prune_requested {scope: prune_scope}`. @p wire binds any
  /// other toolbar controls.
  void build_list_window(
      list_window& lw,
      const char* layout_json,
      const std::string& root_key,
      const std::string& prune_scope,
      const std::string& prune_message,
      const std::function<void(ui_tree&)>& wire = {});

  void rebuild_containers(const bison::dynamic& args);
  void rebuild_images(const bison::dynamic& args);
  void rebuild_volumes(const bison::dynamic& args);
  void rebuild_networks(const bison::dynamic& args);

  /// @brief Re-applies the Containers text filter + state Combo.
  void apply_container_filter();

  /// @brief A row-menu item running @p action on @p e (see run_row_action()).
  common::menu_item action_item(const std::string& label, const entity& e, const std::string& action, bool confirm);
  /// @brief Opens Logs / Inspect for @p e, or emits
  /// `<scope>_action_requested` -- after a confirmation for stop / kill /
  /// remove.
  void run_row_action(const entity& e, const std::string& action);

  void emit_logs_request();
  void emit_inspect_request();

  // ── State ────────────────────────────────────────────────────────────
  std::string title_;

  list_window containers_;
  list_window images_;
  list_window volumes_;
  list_window networks_;

  // Containers-window filter state.
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

  common::text_viewer_panel logs_;
  bison::key_t logs_follow_id_;
  bison::key_t logs_lines_id_;
  std::string open_logs_id_; // container id whose logs the window shows
  bool logs_follow_{false};
  int32_t logs_lines_{500};

  common::text_viewer_panel inspect_;
  std::string open_inspect_id_;
  std::string open_inspect_kind_;

  common::console_panel console_;

  // ── Stats window (live `docker stats` CPU/memory graphs) ─────────────
  //
  // One line per running container in both plots, plus an aggregate
  // "Total" line, and a current-values table.
  std::string stats_root_key_;
  bison::key_t stats_window_id_;
  ui_element_ptr stats_status_label_;
  common::table_rows<> stats_table_;
  common::rolling_plot stats_cpu_;
  common::rolling_plot stats_mem_;

  void build_stats_window();
  /// @brief Fully rebuilds the current-values table (Name / CPU % / Mem % /
  /// Mem Usage), one row per @p args.entries element.
  void rebuild_stats_table(const bison::dynamic& args);
};

/// @brief Register DockerFrontend in the "wish" bison namespace.
void register_docker();

} // namespace bdg::wish
