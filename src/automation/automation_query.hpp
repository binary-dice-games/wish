// MIT License © 2025 Binary Dice Games
/// @file automation_query.hpp
/// @brief Tree/hit-test snapshot builder for the automation query protocol.
///
/// Pure logic, no networking -- mirrors how `draw_protocol.hpp` is a pure,
/// network-independent codec split out of `web_renderer` (see
/// `src/automation/DESIGN.md`). This header has no ImGui dependency either:
/// `hit_test_entry` is a plain-float struct so that `web_renderer` is the
/// only place that ever needs to know `hit_test_map_`'s rects came from
/// `ImGui::GetItemRectMin/Max()`.
#pragma once

#ifdef WISH_AUTOMATION_ENABLED

#include <context/logger.hpp>
#include <ui/ui_importer.hpp>

#include "src/bison/bison_common.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>

namespace bdg::wish::automation {

/**
 * @brief One widget's captured screen rect and interaction flags, as of the
 *        most recently completed frame.
 *
 * Populated by `web_renderer::render_node()` from `ImGui::GetItemRectMin/Max()`
 * / `IsItemHovered()` / `IsItemActive()` / `IsItemVisible()` -- see
 * `src/automation/DESIGN.md`'s "Hit-test capture mechanism".
 */
struct hit_test_entry {
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  bool hovered = false;
  bool active = false;
  bool visible = false;
};

/// @brief Per-widget hit-test rects for one completed frame, keyed by the
///        widget's `__wish_id` field.
using hit_test_map = std::unordered_map<bison::key_t, hit_test_entry, bison::key_t, bison::key_t>;

/// @brief One decoded QUERY_TREE request (see `src/web/draw_protocol.hpp`'s
///        `0x20 QUERY_TREE` message).
struct query_tree_request {
  uint32_t request_id = 0;
  /// Dot-path to restrict the snapshot to (that node and its descendants),
  /// or empty for the whole tree.
  std::string root;
};

/**
 * @brief Parse a QUERY_TREE JSON payload: `{"request_id":N,"root":"..."}`.
 *
 * @param json_payload  Raw UTF-8 JSON text, as returned by
 *                        `draw_protocol::decode_query_tree_message()`.
 * @return `std::nullopt` if the text is not valid JSON or has no numeric
 *         `request_id` field; `root` defaults to `""` (whole tree) when
 *         absent.
 */
std::optional<query_tree_request> parse_query_tree_request(const std::string& json_payload);

/**
 * @brief Build a TREE_SNAPSHOT JSON payload for one query.
 *
 * Walks @p ui_objects (a session's flat dot-path -> element map) *and*, for
 * every element found there, recursively walks its own "children" field for
 * descendants @p ui_objects has no entry for -- i.e. rows/tabs a form
 * appended at runtime via direct `children` map assignment rather than
 * `import_json`'s named-node path (the pattern `ProcessExplorer`'s process
 * table, `Notepad`'s file tabs, and similar forms all use; see
 * `collect_unregistered_descendants()` in automation_query.cpp for the full
 * rationale and how such a descendant's path is synthesized). Both sources
 * are then treated uniformly: optionally restricted to @p root and its
 * descendants, and emitted as one JSON object per widget: `path`, `class`
 * (best-effort, see below), a small set of "well-known" content fields when
 * present on that element (`label`, `text`, `value`, `title`, `checked`,
 * `selected`, `hint` -- whichever exist; no separate per-class schema is
 * maintained), and the hit-test rect/flags joined in from @p hits by
 * `__wish_id`. A widget with no entry in @p hits (never rendered, e.g.
 * inside a collapsed/hidden subtree) gets `rect: null` and
 * `hovered`/`active`/`visible` all `false`.
 *
 * `class` is resolved via `bison::build_display_dict()`, which already maps
 * every registered wish element class's `CLASS` field hash to its name --
 * every `register_*()` in `src/ui/ui_elements/*.cpp` attaches a `DisplayName`
 * attribute equal to its own class name for exactly this purpose (see e.g.
 * `button.cpp`). A class hash with no such attribute registered (e.g. a
 * class defined outside `src/ui/ui_elements/`) falls back to its hash
 * formatted as `"0x########"`.
 *
 * @param request_id  Echoed back verbatim so the browser JS shim can resolve
 *                     the right pending `Promise` (see `window.wish` in
 *                     `resources/embedded/web/client.js`).
 * @param root         Dot-path filter; empty means the whole tree.
 * @param ui_objects   Flat dot-path -> element map for the session being
 *                     queried (`context::ui_objects`).
 * @param hits         Per-widget hit-test rects for the frame just rendered
 *                     (`web_renderer::hit_test_map_`).
 * @return UTF-8 JSON text: `{"request_id":N,"widgets":[...]}`.
 */
std::string build_tree_snapshot(
    uint32_t request_id, const std::string& root, const wish::ui_tree& ui_objects, const hit_test_map& hits);

/**
 * @brief Build a LOG_EVENT JSON payload.
 *
 * Unlike `build_tree_snapshot()`, this has no request/response pairing --
 * it's pushed to every connected browser as soon as new entries exist (see
 * `web_renderer::service_automation_queries()`), so a log line's arrival
 * order tells an automation client exactly where it fell relative to
 * whatever action the client just took (e.g. "click a button, then observe
 * the log entry it caused").
 *
 * Emits one JSON object per entry in @p new_entries, oldest first: `seq`,
 * `timestamp`, `level`, `message` (see `logger::log_entry`).
 *
 * @param new_entries  The subset of `logger::recent_logs()` not yet
 *                      broadcast (entries with `seq` greater than whatever
 *                      was last sent).
 * @return UTF-8 JSON text: `{"logs":[...]}`.
 */
std::string build_log_event(const std::deque<logger::log_entry>& new_entries);

} // namespace bdg::wish::automation

#endif // WISH_AUTOMATION_ENABLED
