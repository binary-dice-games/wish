// MIT License © 2026 Binary Dice Games
/// @file wish_home.hpp
/// @brief `wish::wish_home`: the layout of every file wish keeps for a user
///        under one root (`~/.wish` by default). See src/context/DESIGN.md.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace bdg::wish {

/**
 * @brief Normalizes a session identity into the name of its profile
 *        directory.
 *
 * ASCII letters are lowercased and spaces become `_`, so one user gets one
 * profile on case-sensitive and case-insensitive file systems alike. The
 * result must then be 1 to 64 characters of `[a-z0-9._-]` and must not
 * start with `.`.
 *
 * @return The normalized identity, or empty if @p identity cannot be one
 *         (empty, too long, a path separator, `..`, non-ASCII, ...). An
 *         empty result means the session is anonymous.
 */
std::string normalize_identity(const std::string& identity);

/**
 * @brief Where wish keeps a user's files: one root, one fixed layout.
 *
 * ```
 * <root>/
 *   server/store.bison                  server store
 *   users/<identity>/store.bison        user store
 *   users/<identity>/sandbox/           persistent session sandbox
 *   users/<identity>/desktop/desktop.yaml
 *   logs/                               server.log, standalone.log, desktop/
 *   screenshots/                        server overlay screenshots
 *   traces/                             profiler output
 * ```
 *
 * A plain value: it creates no directory (callers create what they use, on
 * first use) and holds no open files. Per-identity accessors normalize the
 * identity (see `normalize_identity()`) and return an empty path for one
 * that is not valid; callers treat that as an anonymous session.
 * Anonymous sessions never get anything under the root.
 */
class wish_home {
 public:
  /// @brief Uses `default_root()`.
  wish_home();

  /// @param root  The root directory; empty selects `default_root()`.
  explicit wish_home(std::filesystem::path root);

  /**
   * @brief The default root: `$WISH_HOME` if set, else `~/.wish`
   *        (`$HOME`, `%USERPROFILE%` on native Windows), else `.wish` in
   *        the working directory.
   */
  static std::filesystem::path default_root();

  /// @brief The root directory.
  const std::filesystem::path& root() const noexcept {
    return root_;
  }

  /// @brief `<root>/server/store.bison`.
  std::filesystem::path server_store_file() const;

  /// @brief `<root>/users`.
  std::filesystem::path users_dir() const;

  /// @brief `<root>/users/<identity>`; empty for an invalid identity.
  std::filesystem::path user_dir(const std::string& identity) const;

  /// @brief `<user_dir>/store.bison`; empty for an invalid identity.
  std::filesystem::path user_store_file(const std::string& identity) const;

  /// @brief `<user_dir>/sandbox`; empty for an invalid identity.
  std::filesystem::path user_sandbox_dir(const std::string& identity) const;

  /// @brief `<user_dir>/desktop/desktop.yaml`; empty for an invalid identity.
  std::filesystem::path user_desktop_config(const std::string& identity) const;

  /// @brief `<root>/logs`.
  std::filesystem::path logs_dir() const;

  /// @brief `<root>/screenshots`.
  std::filesystem::path screenshots_dir() const;

  /// @brief `<root>/traces`.
  std::filesystem::path traces_dir() const;

  /// @brief Identities with a profile directory under `users_dir()`,
  ///        sorted; directories that are not valid identities are skipped.
  std::vector<std::string> list_identities() const;

 private:
  std::filesystem::path root_;
};

} // namespace bdg::wish
