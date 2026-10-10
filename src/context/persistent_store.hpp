// MIT License © 2025 Binary Dice Games
/// @file persistent_store.hpp
/// @brief File-backed key/value store of bison objects, plus the per-server
///        registry that owns the server store and the per-user stores.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

class persistent_store;
using persistent_store_ptr = std::shared_ptr<persistent_store>;

/**
 * @brief A named collection of bison objects persisted to one file.
 *
 * Each entry is a `bison::dynamic` stored under a string name (e.g.
 * `"bdg.desktop.tail"`). The whole store is loaded once at construction and
 * rewritten to disk after every mutation, so a crash never loses an
 * acknowledged write. Writes are atomic: the new content goes to
 * `<file>.tmp`, which is then renamed over `<file>`.
 *
 * The file uses bison's binary serialization (not JSON), so hashed `key_t`
 * field names round-trip exactly -- `bison::to_json` can only emit names it
 * has a dictionary entry for.
 *
 * Values are deep-copied in and out (`bison::dynamic`'s copy constructor
 * clones nested objects), so a caller mutating a returned value never
 * mutates the store behind its back; write it back with `set()`.
 *
 * Thread-safe: every method may be called concurrently from any thread.
 * Several sessions of the same user share one instance (see
 * `store_registry`), so their writes never overwrite each other's in-memory
 * copy.
 */
class persistent_store {
 public:
  /// @brief Maximum length of an entry name, in bytes.
  static constexpr std::size_t kMaxNameLength = 256;

  /**
   * @brief Open the store backed by @p file, loading it if it exists.
   *
   * Never throws on load: a missing file yields an empty store; an
   * unreadable or corrupt file is renamed to `<file>.corrupt` (so its data
   * isn't silently overwritten by the next write) and the store starts
   * empty. The file and its parent directory are only created on the first
   * write.
   *
   * @param file  Path of the backing file.
   */
  explicit persistent_store(std::filesystem::path file);

  persistent_store(const persistent_store&) = delete;
  persistent_store& operator=(const persistent_store&) = delete;

  /// @brief Path of the backing file.
  const std::filesystem::path& file() const noexcept {
    return file_;
  }

  /**
   * @brief Return a copy of the entry named @p name.
   * @return The entry, or `std::nullopt` if it doesn't exist.
   * @throws std::invalid_argument if @p name is not a valid entry name.
   */
  std::optional<bison::dynamic> get(const std::string& name) const;

  /**
   * @brief Create or replace the entry named @p name and persist the store.
   * @throws std::invalid_argument if @p name is not a valid entry name.
   * @throws std::runtime_error if the store file cannot be written; the
   *         in-memory store is left unchanged in that case.
   */
  void set(const std::string& name, const bison::dynamic& value);

  /**
   * @brief Remove the entry named @p name and persist the store.
   * @return `true` if the entry existed.
   * @throws std::invalid_argument if @p name is not a valid entry name.
   * @throws std::runtime_error if the store file cannot be written.
   */
  bool erase(const std::string& name);

  /**
   * @brief True if an entry named @p name exists.
   * @throws std::invalid_argument if @p name is not a valid entry name.
   */
  bool contains(const std::string& name) const;

  /// @brief Names of every entry, sorted.
  std::vector<std::string> keys() const;

  /**
   * @brief True if @p name is a valid entry name: 1 to `kMaxNameLength`
   *        printable ASCII characters.
   */
  static bool is_valid_name(const std::string& name);

 private:
  using entry_map = std::map<std::string, bison::dynamic>;

  void load(entry_map& entries) const;
  void save(const entry_map& entries) const;

  std::filesystem::path file_;
  bison::synchronized<entry_map> entries_;
};

/**
 * @brief Default directory for persistent stores: `~/.wish`.
 *
 * `~` is `$HOME` (`%USERPROFILE%` on native Windows); falls back to `.wish`
 * relative to the working directory if neither is set.
 */
std::filesystem::path default_store_dir();

/**
 * @brief Owns the persistent stores of one wish server (or standalone host).
 *
 * Layout under the store directory:
 * - `server_store.bison` -- the server store, shared by every session.
 * - `users/<identity>.bison` -- one user store per authenticated identity.
 *
 * Stores are opened lazily, so a server that never touches a store never
 * creates any file. User stores are cached by identity as weak references:
 * concurrent sessions of the same identity share one `persistent_store`,
 * and it is released once the last of them disconnects.
 */
class store_registry {
 public:
  /// @param dir  Directory holding the store files (see class comment).
  explicit store_registry(std::filesystem::path dir);

  /// @brief Directory holding the store files.
  const std::filesystem::path& dir() const noexcept {
    return dir_;
  }

  /// @brief The server store, opened on first use.
  persistent_store_ptr server_store();

  /**
   * @brief The user store for @p identity, opened on first use.
   * @return The store, or null if @p identity is empty or not a single safe
   *         path segment (see `is_safe_sandbox_identity()`) -- such a
   *         session is anonymous and has no user store.
   */
  persistent_store_ptr user_store(const std::string& identity);

 private:
  std::filesystem::path dir_;
  bison::synchronized<persistent_store_ptr> server_store_;
  bison::synchronized<std::unordered_map<std::string, std::weak_ptr<persistent_store>>> user_stores_;
};

using store_registry_ptr = std::shared_ptr<store_registry>;

} // namespace bdg::wish
