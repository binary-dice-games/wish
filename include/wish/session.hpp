// MIT License © 2025 Binary Dice Games
/// @file session.hpp
/// @brief Per-client state container for an active wish session.
#pragma once

#include <wish/ui_importer.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace bdg::wish {

class file_service;
using file_service_ptr = std::shared_ptr<file_service>;

class style_service;
using style_service_ptr = std::shared_ptr<style_service>;

class logger;
using logger_ptr = std::shared_ptr<logger>;

/// @brief Holds all mutable state owned by one connected client.
///
/// Constructed when a client connects; destroyed (and `resource_dir` deleted)
/// when the client disconnects.  Not copyable; moveable so it can live in a
/// container that may rehash.
struct session {
  /// Unique session identifier assigned by the bison RMI layer.
  bison::key_t id;

  /// Flat map of dot-path name → `ui_element_ptr`.  The root node is at key
  /// `""`.  All named descendants follow the dot-joined ancestor naming
  /// convention (e.g. `"body.row.ok"`).  Stored as `ui_tree` so that
  /// imported trees can be merged in directly via `objects.merge()`.
  wish::ui_tree objects;

  /// Named UI blueprint strings (JSON or YAML) registered by the client.
  std::unordered_map<bison::key_t, std::string, bison::key_t, bison::key_t>
      templates;

  /// Sandboxed temporary directory for this session's uploaded resources.
  std::filesystem::path resource_dir;

  /// Application-managed flag; `wish::server` does not read or write it.
  /// Callers may use it for their own throttling or change-detection logic.
  std::atomic<bool> dirty{false};

  /// When `false` (default), widget file paths must be relative and are
  /// sandboxed inside `resource_dir`.  Set to `true` only for same-process
  /// deployments (memory_transport) where absolute host paths are safe.
  /// Controlled by `wish::server::set_allow_absolute_paths()`.
  bool allow_absolute_paths{false};

  /// File service instance; populated by `register_file_service(session&)`.
  file_service_ptr file_service;

  /// Style service instance; holds the client-configured ImGui theme fields.
  /// Read by the renderer before drawing this session's element tree.
  style_service_ptr style_service;

  /// Logger service instance; forwards client log calls to stdout / log file.
  logger_ptr logger_service;

  /// Map of key → root `ui_element_ptr` for every top-level window that the
  /// server must render each frame.  Both template instantiations and form
  /// objects register here:
  ///
  ///  - `template_handler::do_instantiate` adds the root at a unique key.
  ///  - `form::init()` adds the form's internal Window at `internal_root_key_`.
  ///  - `form::remove_internal_objects()` erases by that same key.
  ///
  /// All reads and writes are serialised by `server::sessions_` (the
  /// synchronized session map): the render loop holds the read lock for the
  /// entire frame; every RMI dispatch holds the write lock via the
  /// `on_before_dispatch` / `on_after_dispatch` hooks.
  std::unordered_map<std::string, ui_element_ptr> top_level_objects;

  /// @brief Callback for delivering events to the connected client.
  ///
  /// Parameters: `(object_id, event_name, payload)`.  Null when no client is
  /// attached (e.g. in unit tests that do not require event delivery).
  ///
  /// This function is set once by the server on session creation and is never
  /// replaced.  Use `enqueue_event()` from renderer code — it routes
  /// form-internal events through `widget_event_handlers` and defers all
  /// delivery to after the current frame via `pending_ops`.
  std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit_event;

  /// @brief Per-widget event handlers registered by server-side forms.
  ///
  /// Keyed by the `__wish_id` hash of each internal widget.  Populated by
  /// `form::register_widget_handler` during `on_init`; cleared by
  /// `form::remove_internal_objects`.  The render loop routes events for
  /// registered widgets through these handlers instead of delivering them to
  /// the client.
  std::unordered_map<bison::hash_t,
      std::function<void(bison::key_t /*event*/, bison::dynamic /*payload*/)>>
      widget_event_handlers;

  /// @brief Operations deferred from the current render frame.
  ///
  /// Populated by `enqueue_event()` during rendering; drained by the render
  /// loop after all top-level windows have been drawn and the session lock
  /// has been released.  Each entry is a zero-argument callable that either
  /// invokes a form handler or delivers an event to the client.
  mutable std::vector<std::function<void()>> pending_ops;

  /// @brief Construct a session: creates a unique temporary directory.
  /// @param id  Session identifier; used to derive a unique directory name.
  explicit session(bison::key_t id);

  /// @brief Destroy the session: removes `resource_dir` and all its contents.
  ~session();

  session(const session&) = delete;
  session& operator=(const session&) = delete;
  session(session&& other) = delete;
  session& operator=(session&& other) = delete;
};

// ── Synchronized session wrapper ─────────────────────────────────────────────

/// Synchronized wrapper that owns a session and serialises all access to it
/// with a shared_mutex (multiple concurrent readers or one exclusive writer).
/// All wish server code must access session data through this wrapper.
using sync_session = bison::synchronized<session>;
using sync_session_ptr = std::shared_ptr<sync_session>;

/// Convenience lock-pointer types for the two lock modes.
using sync_session_wlock =
    bison::locked_ptr<session, std::unique_lock<std::shared_mutex>>;
using sync_session_rlock =
    bison::locked_ptr<const session, std::shared_lock<std::shared_mutex>>;

namespace detail {
/// Thread-local pointer to the wish::session whose wlock is currently held by
/// `server::on_before_dispatch`.  Valid only on the worker thread executing an
/// RMI dispatch; null at all other times.
///
/// Form and template-handler methods access session data through this pointer
/// rather than re-acquiring the lock (which would deadlock on a non-recursive
/// mutex).  Do NOT cache or dereference this outside the call stack that entered
/// dispatch.
extern thread_local session* current_session;
}  // namespace detail

/// @brief Write a human-readable dump of every object in the session to @p out.
///
/// Outputs one line per entry in `session.objects` and `session.top_level_objects`,
/// sorted by key, with the element's class type.  Useful for diagnosing missing
/// or unexpected elements in the UI tree.
///
/// Example output:
/// @code
/// wish session objects (6):
///   [DockSpaceViewport]  ""
///   [Window]             "demo_win"
///   [TabBar]             "demo_win.tabs_root"
///   top_level_objects (2):
///   [DockSpaceViewport]  "tpl_0"
///   [Window]             "__form_140703"
/// @endcode
void dump_session_tree(const session& s, std::ostream& out);

/// @brief Enqueue a widget event for deferred dispatch after the current frame.
///
/// Called from renderer code instead of `s.emit_event` directly.  Routing:
///  - If `id` has an entry in `s.widget_event_handlers` the registered form
///    handler is pushed onto `s.pending_ops`.
///  - Otherwise the client-delivery callback (`s.emit_event`) is captured and
///    pushed onto `s.pending_ops`.
///
/// In both cases the actual call happens after the render loop releases the
/// session lock, so handlers are free to modify session state (including
/// `top_level_objects`) without iterator-invalidation or deadlock hazards.
inline void enqueue_event(const session& s,
    bison::key_t id, bison::key_t event, bison::dynamic payload) {
  auto it = s.widget_event_handlers.find(id.id);
  if (it != s.widget_event_handlers.end()) {
    auto handler = it->second;
    s.pending_ops.push_back(
        [handler, event, pl = std::move(payload)]() mutable {
          handler(event, std::move(pl));
        });
  } else if (s.emit_event) {
    auto emit = s.emit_event;
    s.pending_ops.push_back(
        [emit, id, event, pl = std::move(payload)]() mutable {
          emit(id, event, std::move(pl));
        });
  }
}

}  // namespace bdg::wish
