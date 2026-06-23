// MIT License © 2025 Binary Dice Games
/// @file logger.hpp
/// @brief Per-session RMI logging service.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_common.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

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

  void debug(const std::string& msg) { log("debug", msg); }
  void info (const std::string& msg) { log("info",  msg); }
  void warn (const std::string& msg) { log("warn",  msg); }
  void error(const std::string& msg) { log("error", msg); }

 private:
  bool verbose_;
  std::filesystem::path log_path_;
  std::ofstream log_file_;
  std::mutex mtx_;

  /// @brief Format and emit the message; called with `mtx_` held.
  void write_locked(const std::string& level, const std::string& msg);
};

/// @brief Register `"__WishLogger"` in the `"wish"` bison class namespace.
void register_logger();

}  // namespace bdg::wish
