// MIT License © 2026 Binary Dice Games
/// @file notification_service.hpp
/// @brief Per-session RMI service that lets a client post toast
///        notifications on the server's screen.
#pragma once

#include <server/notifications.hpp>

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

class notification_service;
using notification_service_ptr = std::shared_ptr<notification_service>;

/**
 * @brief Gives one client a guarded way to post notifications.
 *
 * Registered in the `"wish"` bison namespace as `"__WishNotifications"`.
 * The server attaches one instance to every session whose renderer shows
 * notifications (see `server::on_session_created()`).
 *
 * ## RMI methods exposed to clients
 *
 * | Method    | Params | Result |
 * |-----------|--------|--------|
 * | `post`    | `"title"`: string (required), `"body"`, `"keys"`, `"tag"`: string, `"kind"`: int32 (0 info, 1 success, 2 warning, 3 error), `"duration_ms"`: int32 (0: until dismissed) | `"id"`: int32 |
 * | `dismiss` | `"id"`: int32 | `"dismissed"`: bool |
 *
 * ## Guards
 *
 * A client can't flood the screen, impersonate the server, or touch other
 * sessions' notifications:
 * - Each notification shows `source` (the session's identity, or
 *   "an anonymous app") under its text.
 * - Text is cut to `kMaxTitle`/`kMaxBody`/`kMaxKeys` bytes, and the
 *   duration to `kMaxDuration` (0 stays 0: until dismissed).
 * - At most `kMaxPerSession` of the session's notifications are on screen;
 *   posting another drops its oldest.
 * - Tags are scoped to the session, and `dismiss` only removes the
 *   session's own notifications.
 * - The session's notifications are removed when it ends.
 */
class notification_service : public bison::dynamic {
 public:
  static constexpr size_t kMaxPerSession = 4;
  static constexpr size_t kMaxTitle = 120;
  static constexpr size_t kMaxBody = 600;
  static constexpr size_t kMaxKeys = 40;
  static constexpr int32_t kMaxDurationMs = 5 * 60 * 1000;

  /**
   * @param center   Where the notifications go (the renderer's); not null.
   * @param source   Shown on every notification ("alice"); empty for an
   *                 anonymous session.
   */
  static notification_service_ptr instantiate(notification_center_ptr center, std::string source);

  notification_service(bison::dynamic&& base, notification_center_ptr center, std::string source);

  /// @brief Removes the session's remaining notifications.
  ~notification_service() override;

  notification_service(const notification_service&) = delete;
  notification_service& operator=(const notification_service&) = delete;

  /// @brief Sets the name shown on the session's next notifications (its
  ///        identity, known only once the session authenticated).
  void set_source(std::string source) {
    *source_.wlock() = std::move(source);
  }

  /// @brief Posts @p n with this session's guards applied (see class doc).
  /// @return Its id. @throws std::invalid_argument for an empty title.
  uint64_t post(notification n);

  /// @brief Removes @p id if this session posted it.
  /// @return Whether it was this session's and still on screen.
  bool dismiss(uint64_t id);

 private:
  notification_center_ptr center_;
  bison::synchronized<std::string> source_;
  std::string tag_prefix_;
  bison::synchronized<std::vector<uint64_t>> ids_; // oldest first
};

/// @brief Registers the `__WishNotifications` class in the `"wish"` namespace.
void register_notification_service();

} // namespace bdg::wish
