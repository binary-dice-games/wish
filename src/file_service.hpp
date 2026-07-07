// MIT License © 2025 Binary Dice Games
/// @file file_service.hpp
/// @brief Sandboxed per-session file store accessible over bison RMI.
#pragma once

#include <context.hpp>

#include "src/bison/bison_object.hpp"

#include <filesystem>
#include <string>

namespace bdg::wish {

/**
 * @brief Typed bison dynamic subclass that implements the file service.
 *
 * Follows the same `dynamic` subclass pattern as `ui_element`: inherits from
 * `bison::dynamic`, constructed from a `dynamic&&` base plus a resource
 * directory path, and exposes typed C++ member functions for all operations.
 *
 * The class is registered as `"__WishFileSystem"_key` in the `"wish"` namespace via
 * `register_file_service()`.  One instance is created per session by
 * `server::on_session_created`.
 */
class file_service : public bison::dynamic {
 public:
  /**
   * @brief Instatitates a new file service instance.
   * @param resource_dir  Sandboxed directory for this session's files.
   */
  static file_service_ptr instantiate(std::filesystem::path resource_dir);

  /**
   * @brief Construct from a bison base object and a resource directory.
   * @param base          Plain `dynamic` produced by `dynamic::instantiate`.
   * @param resource_dir  Sandboxed directory for this session's files.
   */
  file_service(bison::dynamic&& base, std::filesystem::path resource_dir);

  /**
   * @brief Write @p data to a file named @p name in the resource directory.
   * @throws std::runtime_error if @p name contains path separators or `..`,
   *         or if the resource directory does not exist.
   */
  void upload(const std::string& name, const std::string& data);

  /**
   * @brief Read and return the contents of a previously uploaded file.
   * @throws std::runtime_error if the file does not exist or cannot be read.
   */
  std::string download(const std::string& name) const;

  /**
   * @brief Return an indexed `dynamic` whose entries are the uploaded file
   *        names (one per slot, in filesystem iteration order).
   */
  bison::dynamic_ptr list() const;

  /**
   * @brief Remove a previously uploaded file.
   * @throws std::runtime_error if the file does not exist.
   */
  void erase(const std::string& name);

  /**
   * @brief Validate and resolve @p name against @p resource_dir.
   *
   * Relative paths are resolved purely syntactically (no filesystem access),
   * so the function works for files that do not yet exist.  The resolved path
   * must remain inside @p resource_dir; paths that escape via `..` or similar
   * are rejected.
   *
   * Absolute paths are accepted only when @p allow_absolute is `true`; this
   * flag should only be set for same-process (`memory_transport`) deployments.
   *
   * @param name           Raw path value (relative or absolute).
   * @param resource_dir   Session sandbox directory.
   * @param allow_absolute Whether absolute paths are permitted.
   * @return Resolved path, or an empty path if @p name is rejected.
   */
  static std::filesystem::path
  resolve_path(const std::string& name, const std::filesystem::path& resource_dir, bool allow_absolute = false);

 private:
  std::filesystem::path resource_dir_;

  /// @brief Resolve @p name relative to `resource_dir_`, throwing if the
  ///        result would escape the sandbox.
  std::filesystem::path resolve_path(const std::string& name) const;
};

/// @brief Register `"__WishFileSystem"_key` in the `"wish"` bison namespace.
///        Called once by `register_all()`; idempotent across repeated calls.
void register_file_service();

} // namespace bdg::wish
