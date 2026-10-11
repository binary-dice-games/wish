// MIT License © 2026 Binary Dice Games
/**
 * @file process_launcher.hpp
 * @brief Starts and tracks the app processes `wish desktop` launches from
 *        its Start menu, on libuv (no platform-specific code).
 */
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish {

/// @brief One process to start.
struct launch_request {
  /// Program and arguments; argv[0] is the executable path.
  std::vector<std::string> argv;
  /// Environment variables set (or replaced) on top of this process's own.
  std::vector<std::pair<std::string, std::string>> env;
  /// File receiving the child's stdout and stderr (truncated). Empty
  /// discards them.
  std::filesystem::path log_path;
};

/// @brief What `wish_desktop` needs from a process launcher; a seam for
///        tests.
class process_launcher_iface {
 public:
  virtual ~process_launcher_iface() = default;

  /**
   * @brief Starts @p request's program without waiting for it.
   * @return The child's pid, or nullopt (with @p error set) if it could not
   *         be started.
   */
  virtual std::optional<int> launch(const launch_request& request, std::string& error) = 0;

  /// @brief Asks every still-running child to terminate (SIGTERM, or
  ///        TerminateProcess on Windows). Does not wait.
  virtual void terminate_all() = 0;
};

/**
 * @brief libuv-based launcher: owns a private event loop on its own thread,
 *        which spawns the children (`uv_spawn`) and reaps them as they exit.
 *
 * Children are not detached; the destructor terminates any still running.
 */
class process_launcher : public process_launcher_iface {
 public:
  process_launcher();
  ~process_launcher() override;

  process_launcher(const process_launcher&) = delete;
  process_launcher& operator=(const process_launcher&) = delete;

  std::optional<int> launch(const launch_request& request, std::string& error) override;
  void terminate_all() override;

  /// @brief Pids of the children that have not exited yet.
  std::vector<int> running() const;

  /// @brief Absolute path of the running executable, or empty on failure.
  static std::filesystem::path self_exe_path();

 private:
  struct impl;
  std::unique_ptr<impl> impl_;
};

} // namespace bdg::wish
