// MIT License © 2025 Binary Dice Games
/// @file session.hpp
/// @brief Per-client state container for an active wish session.
#pragma once

#include <wish/ui_importer.hpp>

#include "src/bison/bison_common.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace bdg::wish {

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

  /// Set to `true` by the `__setter` hook whenever a field changes; cleared
  /// after each render frame.
  std::atomic<bool> dirty{false};

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

}  // namespace bdg::wish
