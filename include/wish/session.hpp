// MIT License © 2025 Binary Dice Games
/// @file session.hpp
/// @brief Per-client state container for an active wish session.
#pragma once

#include <wish/ui_importer.hpp>

#include "src/bison/bison_common.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

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
  /// convention (e.g. `"body.row.ok"`).
  wish::name_map objects;

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

  /// Guards `top_level_objects`.  The render thread snapshots the map under
  /// this lock; writers (form lifecycle, template instantiation) hold it while
  /// inserting or erasing entries.
  mutable std::mutex top_level_mutex;

  /// Map of key → root `ui_element_ptr` for every top-level window that the
  /// server must render each frame.  Both template instantiations and form
  /// objects register here:
  ///
  ///  - `template_handler::do_instantiate` adds the root at a unique key.
  ///  - `form::init()` adds the form's internal Window at `internal_root_key_`.
  ///  - `form::remove_internal_objects()` erases by that same key.
  ///
  /// The render loop iterates all values without relying on the `""` convention
  /// used in `objects`, so multiple top-level windows coexist correctly.
  /// Always access under `top_level_mutex`.
  std::unordered_map<std::string, ui_element_ptr> top_level_objects;

  /// @brief Callback for emitting asynchronous events to the connected client.
  ///
  /// Parameters: `(object_id, event_name, payload)`.  Null when no client is
  /// attached (e.g. in unit tests that do not require event delivery).
  std::function<void(bison::key_t, bison::key_t, bison::dynamic)> emit_event;

  /// @brief Construct a session: creates a unique temporary directory.
  /// @param id  Session identifier; used to derive a unique directory name.
  explicit session(bison::key_t id);

  /// @brief Destroy the session: removes `resource_dir` and all its contents.
  ~session();

  session(const session&) = delete;
  session& operator=(const session&) = delete;

  /// @brief Move construction transfers `resource_dir` ownership.
  ///
  /// After the move, the source session's `resource_dir` is empty so its
  /// destructor will not attempt to delete the directory.
  session(session&& other) noexcept;
  session& operator=(session&& other) noexcept;
};

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

}  // namespace bdg::wish
