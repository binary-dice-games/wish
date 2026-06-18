// MIT License © 2025 Binary Dice Games
/// @file file_service.hpp
/// @brief Sandboxed per-session file store accessible over bison RMI.
#pragma once

#include <wish/session.hpp>

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
 * The class is registered as `"__WishFS"_key` in the `"wish"` namespace via
 * `register_file_service()`.  One instance is created per session by
 * `server::on_session_created`.
 */
class file_service_node : public bison::dynamic {
 public:
  /**
   * @brief Construct from a bison base object and a resource directory.
   * @param base          Plain `dynamic` produced by `dynamic::instantiate`.
   * @param resource_dir  Sandboxed directory for this session's files.
   */
  file_service_node(bison::dynamic&& base,
                    std::filesystem::path resource_dir);

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

 private:
  std::filesystem::path resource_dir_;

  /// @brief Resolve @p name relative to `resource_dir_`, throwing if the
  ///        result would escape the sandbox.
  std::filesystem::path resolve_path(const std::string& name) const;
};

/// @brief Register `"__WishFS"_key` in the `"wish"` bison namespace.
///        Called once by `register_all()`; idempotent across repeated calls.
void register_file_service();

}  // namespace bdg::wish
