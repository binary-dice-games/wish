// MIT License © 2026 Binary Dice Games
/// @file notifications.hpp
/// @brief Toast notifications ("Press Shift+F1 to open the overlay") shown
///        by the host renderer in a corner of the server's screen.
///
/// The model is renderer-agnostic and thread-safe: any server-side code
/// (the server app, a form via `context::notifications`, a background
/// thread) posts a notification, and the renderer that owns the
/// `notification_center` draws the active ones each frame (see
/// `src/imgui/imgui_notifications.hpp` for the ImGui drawing).
#pragma once

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Severity of a notification; selects its accent color and icon.
enum class notification_kind { info, success, warning, error };

/// @brief What to show in one notification.
struct notification {
  std::string title; ///< Bold first line; required.
  std::string body; ///< Optional text under the title.
  /// Optional key combination drawn as key caps after the body, e.g.
  /// "Shift+F1" (split at '+').
  std::string keys;
  notification_kind kind{notification_kind::info};
  /// How long the notification stays; zero keeps it until dismissed.
  std::chrono::milliseconds duration{std::chrono::seconds{6}};
  /// Who posted it, shown small under the text (e.g. "From alice"); empty
  /// for the server itself. Set by the server for client notifications,
  /// so an app can't pass its toasts off as the server's.
  std::string source;
  /// Optional tag: posting a notification with the tag of an active one
  /// replaces that one instead of stacking a second copy (e.g. a "server
  /// overlay" hint posted twice).
  std::string tag;
};

/// @brief A posted notification, as returned by `notification_center::active()`.
struct active_notification {
  uint64_t id{0}; ///< Handle for `notification_center::dismiss()`.
  notification content;
  std::chrono::steady_clock::time_point posted; ///< When it was (re)posted.
  /// When it expires; `time_point::max()` for a sticky notification.
  std::chrono::steady_clock::time_point expires;
};

/**
 * @brief Thread-safe list of the notifications currently on screen.
 *
 * `post()` and `dismiss()` may be called from any thread. The renderer
 * calls `active()` once per frame, which also drops expired notifications,
 * and `consume_changed()` to learn that a frame must be drawn even though
 * no session is dirty (e.g. the first notification posted at startup).
 */
class notification_center {
 public:
  /// @brief Most notifications kept at once; posting more drops the oldest.
  static constexpr size_t kMaxActive = 16;

  /**
   * @brief Shows @p n until its duration elapses or it is dismissed.
   * @return Its id. When @p n has the tag of an active notification, that
   *         one is replaced (and keeps its id).
   */
  uint64_t post(notification n);

  /// @brief Removes notification @p id; no-op when it is already gone.
  void dismiss(uint64_t id);

  /// @brief Removes every notification.
  void clear();

  /// @brief The notifications not yet expired at @p now, oldest first;
  ///        expired ones are dropped.
  std::vector<active_notification> active(
      std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /// @brief True (once) when a notification was posted or removed since
  ///        the last call.
  bool consume_changed() {
    return changed_.exchange(false, std::memory_order_acq_rel);
  }

 private:
  bison::synchronized<std::vector<active_notification>> items_;
  std::atomic<uint64_t> next_id_{1};
  std::atomic<bool> changed_{false};
};

using notification_center_ptr = std::shared_ptr<notification_center>;

} // namespace bdg::wish
