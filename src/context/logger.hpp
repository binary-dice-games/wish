// MIT License © 2025 Binary Dice Games
/// @file logger.hpp
/// @brief Per-session RMI logging service.
#pragma once

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#ifdef WISH_AUTOMATION_ENABLED
#include <cstdint>
#include <deque>
#endif

namespace bdg::wish {

/**
 * @brief Server log verbosity, ordered from least to most verbose.
 *
 * Selected by the `--verbose` CLI flag (default `none`).  Two thresholds
 * matter:
 *  - `>= info`  : RMI request/response trace lines and session lifecycle
 *                 lines are produced (and mirrored to stdout).
 *  - `>= trace` : those trace lines additionally carry decoded payloads
 *                 (`args=`, `set` values, response bodies).
 *
 * `fatal`/`error`/`warning` produce no RMI trace output at all; they only
 * raise the severity floor for client `logger.log()` messages written to the
 * log file.  `none` writes nothing.
 */
enum class log_level { none = 0, fatal, error, warning, info, trace };

/// @brief Parse a level name (case-insensitive). Accepts `warn` for
///        `warning` and `debug` for `trace`. Returns `std::nullopt` if
///        @p s names no level.
std::optional<log_level> parse_log_level(std::string_view s);

/// @brief Canonical lowercase name of @p lvl (e.g. `"info"`).
std::string_view to_string(log_level lvl);

class logger;
using logger_ptr = std::shared_ptr<logger>;

/**
 * @brief Per-session service that lets clients send structured log messages.
 *
 * Registered in the `"wish"` bison namespace as `"__WishLogger"`.  The server
 * creates one instance per connected client in `on_session_created`; the client
 * reaches it via `wish::client` helper methods.
 *
 * The configured @ref log_level gates output: a message is written to the
 * log file only when the logger is verbose enough for its severity, and is
 * additionally mirrored to `std::cout` when the level is `info` or higher.
 * At `none` nothing is written.  A log file at @p log_path is opened
 * regardless of level (so it exists once the server starts); if @p log_path
 * is empty no file is opened.
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
   * @param level     Verbosity floor; see @ref log_level.
   * @param log_path  File to append messages to; ignored when empty.
   */
  logger(bison::dynamic&& base, log_level level, std::filesystem::path log_path);

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

  /// @brief The configured verbosity floor.
  log_level level() const noexcept {
    return level_;
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
   * Thread-safe: copies the buffer under `state_`'s read lock.
   */
  std::deque<log_entry> recent_logs() const;
#endif

 private:
  log_level level_;
  std::filesystem::path log_path_;

  /// @brief Mutable state shared across `log()` calls, guarded by `state_`.
  struct state {
    std::ofstream log_file;
#ifdef WISH_AUTOMATION_ENABLED
    std::deque<log_entry> recent_logs;
    uint64_t next_log_seq = 1;
#endif
  };
  mutable bison::synchronized<state> state_;

  /// @brief Format and emit the message; called with `state_`'s write lock held.
  void write_locked(state& s, const std::string& level, const std::string& msg);

#ifdef WISH_AUTOMATION_ENABLED
  /// @brief Append a `log_entry` to `s.recent_logs`; called with `state_`'s write lock held.
  void record_for_automation(state& s, const std::string& level, const std::string& msg);

  static constexpr size_t kMaxRecentLogs = 200;
#endif
};

/// @brief Register `"__WishLogger"` in the `"wish"` bison class namespace.
void register_logger();

} // namespace bdg::wish
