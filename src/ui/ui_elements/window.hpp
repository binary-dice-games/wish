// MIT License © 2025 Binary Dice Games
/// @file window.hpp
/// @brief Concrete ui_root for the "Window" bison class.
#pragma once

#include <ui/ui_root.hpp>

#include <optional>

namespace bdg::wish {

/// @brief Typed C++ class for wish Window elements.
///
/// Sits at the end of the primary inheritance chain:
///   `bison::dynamic ← ui_element ← ui_root ← window`
///
/// Because `window` inherits `ui_root`, only Window elements (not generic
/// ui_elements) are registered in `context::top_level_handlers` and receive
/// `on_event` callbacks from the render loop.
class window : public cloneable_ui_element<window, ui_root> {
 public:
  explicit window(bison::dynamic&& base);

  /// @brief Cached `get_as<bool>("closable"_key, def)`.
  bool closable(bool def = false) const {
    return cached_field_or<bool>(closable_field_, bison::key_t{"closable"}, def);
  }

  /// @brief Cached `get_as<int32_t>("flags"_key, def)`.
  int32_t flags(int32_t def = 0) const {
    return cached_field_or<int32_t>(flags_field_, bison::key_t{"flags"}, def);
  }

  /// @brief Cached `get_as<std::string>("title"_key, def)`.
  std::string title(std::string def = {}) const {
    return cached_field_or<std::string>(title_field_, bison::key_t{"title"}, std::move(def));
  }
  /// @brief Zero-copy form of `title()` (empty fallback) -- see
  ///        `ui_element::cached_field_str()`. `render_window()` reads this
  ///        every frame.
  const std::string& title_ref() const { return cached_field_str(title_field_, bison::key_t{"title"}); }

  /// @brief Cached `get_as<bool>("modal"_key, def)`.
  bool modal(bool def = false) const {
    return cached_field_or<bool>(modal_field_, bison::key_t{"modal"}, def);
  }

  /// @brief Cached `get_as<int32_t>("pos_x"_key, def)`.
  int32_t pos_x(int32_t def = -1) const {
    return cached_field_or<int32_t>(pos_x_field_, bison::key_t{"pos_x"}, def);
  }

  /// @brief Cached `get_as<int32_t>("pos_y"_key, def)`.
  int32_t pos_y(int32_t def = -1) const {
    return cached_field_or<int32_t>(pos_y_field_, bison::key_t{"pos_y"}, def);
  }

  /// @brief One-shot modal-open latch: true once `ImGui::OpenPopup()` has
  /// already been called for this popup, false once it has fully closed.
  /// Replaces the old `"__modal_opened__"` hidden field -- this state is
  /// pure per-frame render bookkeeping never read outside render_window(),
  /// so it doesn't need to be a bison::dynamic field (and, unlike that
  /// field, is correctly *not* copied by clone()).
  bool modal_opened() const { return modal_opened_; }
  void set_modal_opened(bool v) const { modal_opened_ = v; }

  /// @brief True if @p is_collapsed differs from the collapse state
  /// recorded on the previous call (or this is the first call); always
  /// updates the recorded state to @p is_collapsed. Replaces
  /// `"__was_collapsed__"`.
  bool collapse_transitioned(bool is_collapsed) const {
    bool was = was_collapsed_.value_or(is_collapsed);
    was_collapsed_ = is_collapsed;
    return was != is_collapsed;
  }

  /// @brief True exactly on the frame @p is_docked transitions from true to
  /// false -- the frame render_window() should restore the last-known
  /// floating size via float_width()/float_height(). Always updates the
  /// recorded docked state to @p is_docked. Replaces `"__was_docked__"`.
  bool just_undocked(bool is_docked) const {
    bool was = was_docked_.value_or(is_docked);
    was_docked_ = is_docked;
    return was && !is_docked;
  }

  /// @brief Last floating (undocked) window size recorded by
  /// set_float_size(), or 0 if the window has never been floating.
  /// Replaces `"__float_width__"`/`"__float_height__"`.
  int32_t float_width() const { return float_width_; }
  int32_t float_height() const { return float_height_; }
  void set_float_size(int32_t w, int32_t h) const {
    float_width_ = w;
    float_height_ = h;
  }

 private:
  mutable bison::field* closable_field_ = nullptr;
  mutable bison::field* flags_field_ = nullptr;
  mutable bison::field* title_field_ = nullptr;
  mutable bison::field* modal_field_ = nullptr;
  mutable bison::field* pos_x_field_ = nullptr;
  mutable bison::field* pos_y_field_ = nullptr;

  mutable bool modal_opened_ = false;
  mutable std::optional<bool> was_collapsed_;
  mutable std::optional<bool> was_docked_;
  mutable int32_t float_width_ = 0;
  mutable int32_t float_height_ = 0;
};

} // namespace bdg::wish
