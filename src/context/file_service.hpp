// MIT License © 2025 Binary Dice Games
/// @file file_service.hpp
/// @brief Sandboxed per-session file store accessible over bison RMI.
#pragma once

#include <context/context.hpp>

#include "src/bison/bison_object.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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
   * @brief Write one chunk of a file being uploaded in pieces.
   *
   * Stateless on the server: chunks are written to a staging file
   * (`<resolved path>.wishpart`) and only become visible at @p name once the
   * chunk with @p eof set arrives. @p first must be `true` for exactly the
   * first chunk of a transfer (creates/truncates the staging file) and
   * `false` for every subsequent chunk (appends); an empty file is produced
   * by a single call with both @p first and @p eof set to `true` and an
   * empty @p data.
   *
   * @param name   Target filename, sandboxed the same as `upload()`.
   * @param data   Chunk bytes; may contain embedded NUL bytes.
   * @param first  `true` for the first chunk of the transfer.
   * @param eof    `true` for the last chunk; triggers the atomic rename from
   *               the staging file onto @p name.
   * @throws std::runtime_error if @p name escapes the sandbox or the
   *         staging file cannot be written.
   */
  void upload_chunk(const std::string& name, const std::string& data, bool first, bool eof);

  /**
   * @brief One chunk of a file being downloaded in pieces.
   */
  struct chunk {
    std::string data; ///< Bytes read, up to the requested chunk size.
    bool eof = false; ///< `true` once `offset + data.size()` reached EOF.
  };

  /**
   * @brief Read one chunk of a previously uploaded file at a given offset.
   *
   * Stateless: each call independently seeks to @p offset and reads, so
   * calls may be retried or interleaved with other operations without any
   * server-side handle to leak or clean up.
   *
   * @param name      Filename to read, sandboxed the same as `download()`.
   * @param offset    Byte offset to start reading from.
   * @param max_size  Maximum number of bytes to read.
   * @return The bytes read (possibly empty) and whether EOF was reached.
   * @throws std::runtime_error if the file does not exist or cannot be read.
   */
  chunk download_chunk(const std::string& name, int32_t offset, int32_t max_size) const;

  /**
   * @brief Extract a zip archive previously uploaded via `upload_chunk()`
   *        into a sandboxed destination directory.
   *
   * Both @p zip_name and @p dest are resolved and sandboxed independently
   * (same rules as `upload()`/`download()`). Every extracted entry's target
   * path is additionally verified to stay within the resolved @p dest
   * directory (zip-slip protection: unlike the build-controlled embedded
   * resource archive, @p zip_name's entries are client-supplied content and
   * must not be trusted). Extraction merges into @p dest -- existing files
   * are overwritten, unrelated existing files are left alone. On success,
   * the staging archive at @p zip_name is deleted.
   *
   * @param zip_name  Previously uploaded zip file, relative to the sandbox.
   * @param dest      Destination directory, relative to the sandbox.
   * @throws std::runtime_error if either path escapes the sandbox, the
   *         archive cannot be opened, or any entry would extract outside
   *         @p dest.
   */
  void unpack(const std::string& zip_name, const std::string& dest);

  /**
   * @brief Return an indexed `dynamic` whose entries are the file names found
   *        directly under @p path (one per slot, in filesystem iteration
   *        order; not recursive).
   * @param path  Subdirectory relative to the resource directory, e.g.
   *              `"res/icons"`. Empty (the default) lists the resource
   *              directory's own top-level contents.
   * @throws std::runtime_error if @p path escapes the sandbox (same
   *         validation as `upload`/`download`/`erase`).
   */
  bison::dynamic_ptr list(const std::string& path = "") const;

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

  /**
   * @brief Like `resolve_path()`, but also accepts an `http://`/`https://`
   *        URL in @p name, fetching it into the sandbox on demand.
   *
   * Dispatches on @p name's prefix:
   * - `http://`/`https://`: rejected outright (empty path, no network
   *   request made) unless @p allow_fetch is `true` and its URL path's
   *   extension case-insensitively matches one of @p allowed_extensions.
   *   Otherwise, resolves to
   *   `resource_dir/url_cache/<hash of name>.<ext>`. If that file already
   *   exists, it is returned immediately. Otherwise a background download is
   *   started (unless one for this exact URL is already in flight, or a
   *   previous attempt already failed) and an empty path is returned for
   *   this call -- callers must treat that identically to "resource not
   *   available yet" (matching what every current caller already does for
   *   any other unresolved path) and simply call again on a later frame.
   *   This function never blocks on network I/O.
   * - `file://`: the remainder is treated as an absolute local path, subject
   *   to the same @p allow_absolute rule as `resolve_path()`.
   * - Otherwise: identical to `resolve_path(name, resource_dir,
   *   allow_absolute)`.
   *
   * @param name                Raw path or URL value.
   * @param resource_dir        Session sandbox directory.
   * @param allow_absolute      Whether absolute (or `file://`) paths are permitted.
   * @param allow_fetch         Whether `http://`/`https://` URLs may be downloaded
   *                            at all; when `false`, every such URL is rejected
   *                            without inspecting its extension or touching the
   *                            network. Should be wired to
   *                            `wish::server::set_allow_url_fetch()`'s value.
   * @param allowed_extensions  Case-insensitive extensions (without the leading
   *                            '.') a URL's path may end in to be fetched at all.
   * @return Resolved local path, or an empty path if @p name is rejected,
   *         still downloading, or previously failed to download.
   */
  static std::filesystem::path resolve_or_fetch(
      const std::string& name,
      const std::filesystem::path& resource_dir,
      bool allow_absolute,
      bool allow_fetch,
      const std::vector<std::string>& allowed_extensions);

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
