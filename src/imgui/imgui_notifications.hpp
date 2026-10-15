// MIT License © 2026 Binary Dice Games
/// @file imgui_notifications.hpp
/// @brief Draws a `notification_center`'s active notifications as toasts
///        stacked in a corner of the main viewport (ImGui backends).
#pragma once

#include <server/notifications.hpp>

#include <imgui.h>

#include <string>
#include <vector>

namespace bdg::wish {

/// @brief Corner of the main viewport's work area the toasts stack from.
enum class notification_corner { top_left, top_right, bottom_left, bottom_right };

/**
 * @brief Draws the active notifications of @p center as toasts, newest
 *        nearest the corner, on top of every other window.
 *
 * Call once per frame inside an ImGui frame, after everything the toasts
 * should cover (see `renderer::render_server_overlay()`). Each toast fades
 * and slides in, fades out before it expires, shows its remaining time as
 * a thin bar, and is dismissed by a click. At most `kMaxVisible` are drawn;
 * older ones wait their turn.
 *
 * Uses the main viewport's work area, so a viewport side bar such as the
 * desktop's taskbar is never covered.
 *
 * @return true while at least one toast is on screen: the caller must keep
 *         drawing frames for the animations and expiry to progress.
 */
bool render_notifications(notification_center& center, notification_corner corner = notification_corner::bottom_right);

/// @brief Most toasts drawn at once.
inline constexpr size_t kMaxVisibleNotifications = 4;

namespace detail {

/// @brief Splits a key combination ("Ctrl+Shift+F1") into key cap labels
///        ({"Ctrl", "Shift", "F1"}); a trailing "+" is the plus key
///        ("Ctrl++" -> {"Ctrl", "+"}). Empty parts are dropped.
std::vector<std::string> split_key_caps(const std::string& keys);

/// @brief Opacity (0..1) of a toast @p age_s seconds after it was posted
///        with @p remaining_s seconds left (negative: sticky): fades in
///        over the first 0.2 s and out over the last 0.5 s.
float notification_alpha(float age_s, float remaining_s);

} // namespace detail

} // namespace bdg::wish
