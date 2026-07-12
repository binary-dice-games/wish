// MIT License © 2025 Binary Dice Games
/// @file logger.hpp
/// @brief Per-session RMI logging service.
#pragma once

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef WISH_AUTOMATION_ENABLED
#include <cstdint>
#include <deque>
#endif

namespace bdg::wish {

class logger;
using logger_ptr = std::shared_ptr<logger>;

/**
 * @brief Per-session service that lets clients send structured log messages.
 *
 * Registered in the `"wish"` bison namespace as `"__WishLogger"`.  The server
 * creates one instance per connected client in `on_session_created`; the client
 * reaches it via `wish::client` helper methods.
 *
 * When @p verbose is `true` each message is also written to `std::cout`.
 * A log file at @p log_path is always written (truncated on construction).
 * If @p log_path is empty no file is opened.
 *
 * ## RMI method exposed to clients
 *
 * | Method | Params                              | Effect                   |
 * |--------|-------------------------------------|--------------------------|
 * | `log`  | `"level"`: string, `"msg"`: string  | Appends a log entry      |
 *
 * Valid level strings: `"debug"`, `"info"`, `"warn"`, `"error"`.
 *
 * When built with `WISH_ENABLE_AUTOMATION`, every `log()` call is also
 * recorded (bounded, see `recent_logs()`) and broadcast live to any
 * connected automation client as a LOG_EVENT (`src/web/draw_protocol.hpp`).
 * Because delivery is push-based and ordered, an automation script sees log
 * events interleaved with its own actions in the order they actually
 * happened -- e.g. "click a button, then observe the log entry it caused"
 * -- with no need to correlate timestamps or UI state after the fact. See
 * `CLAUDE.md`'s "Automation" section.
 */
class logger : public bison::dynamic {
 public:
  /**
   * @brief Construct and register the RMI `log` method.
   * @param base      Prototype-initialised dynamic base (from `dynamic::instantiate`).
   * @param verbose   When true, mirror every message to `std::cout`.
   * @param log_path  File to append messages to; ignored when empty.
   */
  logger(bison::dynamic&& base, bool verbose, std::filesystem::path log_path);

  // ── Public C++ API ────────────────────────────────────────────────────────────

  /// @brief Write a log entry.
  /// @param level  Severity label (`"debug"`, `"info"`, `"warn"`, `"error"`).
  /// @param msg    Free-form message text.
  void log(const std::string& level, const std::string& msg);

  void debug(const std::string& msg) {
    log("debug", msg);
  }
  void info(const std::string& msg) {
    log("info", msg);
  }
  void warn(const std::string& msg) {
    log("warn", msg);
  }
  void error(const std::string& msg) {
    log("error", msg);
  }

  /// @brief Returns true when verbose mode is on (stdout mirroring enabled).
  bool is_verbose() const noexcept {
    return verbose_;
  }

#ifdef WISH_AUTOMATION_ENABLED
  // ── automation ───────────────────────────────────────────────────────────

  /**
   * @brief One buffered `log()` call.
   *
   * `seq` is what lets `web_renderer::service_automation_queries()` (which
   * polls `recent_logs()` once per frame) broadcast each entry exactly
   * once, as a LOG_EVENT, in the order `log()` was called -- see
   * `src/automation/DESIGN.md`.
   */
  struct log_entry {
    uint64_t seq = 0; ///< Monotonically increasing per session; never reused.
    std::string timestamp; ///< Same `"%Y-%m-%d %H:%M:%S"` format as the log file.
    std::string level;
    std::string message;
  };

  /**
   * @brief Snapshot of the most recent `log()` calls (oldest dropped first
   *        once the buffer exceeds `kMaxRecentLogs`).
   *
   * Thread-safe: copies the buffer under the same `mtx_` that serializes
   * `log()` itself.
   */
  std::deque<log_entry> recent_logs() const;
#endif

 private:
  bool verbose_;
  std::filesystem::path log_path_;
  std::ofstream log_file_;
  mutable std::mutex mtx_;

  /// @brief Format and emit the message; called with `mtx_` held.
  void write_locked(const std::string& level, const std::string& msg);

#ifdef WISH_AUTOMATION_ENABLED
  /// @brief Append a `log_entry` to `recent_logs_`; called with `mtx_` held.
  void record_for_automation(const std::string& level, const std::string& msg);

  std::deque<log_entry> recent_logs_;
  uint64_t next_log_seq_ = 1;
  static constexpr size_t kMaxRecentLogs = 200;
#endif
};

/// @brief Register `"__WishLogger"` in the `"wish"` bison class namespace.
void register_logger();

} // namespace bdg::wish
