// MIT License © 2026 Binary Dice Games
/// @file imgui_keyboard.hpp
/// @brief Keyboard-only control helpers: Windows-style access keys
///        ("&File" -> underlined F, Alt+F) and shortcut strings
///        ("Ctrl+S", "F5") parsed into ImGui key chords.
///
/// The actual navigation (Tab/Shift+Tab, arrows, Space/Enter, Esc) is Dear
/// ImGui's own keyboard nav, enabled by enable_keyboard_navigation(). This
/// module only adds what ImGui lacks: access-key labels and a text syntax for
/// shortcuts. See src/imgui/DESIGN.md, "Keyboard control".
#pragma once

#include <imgui.h>

#include <string>
#include <string_view>

namespace bdg::wish {

/// @brief Turns on Dear ImGui's keyboard navigation for a renderer's context.
///        Called once by every windowed/web renderer's setup().
void enable_keyboard_navigation(ImGuiIO& io);

/// @brief A label with its access-key markup resolved.
struct access_key_label {
  /// Label to hand to ImGui: the caller's own string when it has no markup,
  /// otherwise the `storage` passed to parse_access_key(). Keeps any "##id"
  /// suffix verbatim.
  const char* text = "";
  /// Byte offset of the access-key character within @ref text; -1 if none.
  int index = -1;
  /// Key that triggers it (ImGuiKey_A..Z / 0..9), ImGuiKey_None if none.
  ImGuiKey key = ImGuiKey_None;

  bool has_key() const {
    return key != ImGuiKey_None;
  }
};

/// @brief Resolves Windows-style access-key markup in @p label.
///
/// In the visible part of the label (before any "##"): "&&" is a literal
/// '&'; the first '&' followed by an ASCII letter or digit is removed and
/// marks that character as the access key; any other '&' (e.g. "Save & Exit")
/// stays as-is.
///
/// @param label    Raw label text.
/// @param storage  Buffer that receives the stripped label when @p label has
///                 markup; must outlive the returned value's use.
/// @return The label to display and its access key. Never throws.
access_key_label parse_access_key(const std::string& label, std::string& storage);

/// @brief Parses a shortcut string such as "Ctrl+S", "Ctrl+Shift+N", "F5",
///        "Alt+F4", "Del" or "Ctrl+Plus" (case-insensitive).
///
/// Modifiers: Ctrl/Control, Shift, Alt, Super/Win/Cmd. Keys: A-Z, 0-9,
/// F1-F24, Enter/Return, Esc/Escape, Tab, Space, Backspace, Del/Delete,
/// Ins/Insert, Home, End, PgUp/PageUp, PgDn/PageDown, Up/Down/Left/Right,
/// Plus/Minus, the punctuation keys - = , . / ; ' [ ] \ ` and the numeric
/// keypad: Num0-Num9, NumPlus, NumMinus, NumMul, NumDiv, NumDot, NumEnter.
///
/// @return The ImGui key chord, or 0 when the string is empty or invalid
///         (unknown token, no non-modifier key, more than one key).
ImGuiKeyChord parse_key_chord(std::string_view text);

/// @brief Where a shortcut is active.
enum class shortcut_scope {
  window, ///< While the window it is rendered in (or a child/docked window) has keyboard focus.
  app, ///< While any window of the same session (app) has keyboard focus.
};

/// @brief "app" -> shortcut_scope::app; anything else (including "") ->
///        shortcut_scope::window.
shortcut_scope parse_shortcut_scope(std::string_view text);

/// @brief True on the frame @p chord is pressed within @p scope. For
///        shortcut_scope::app the caller passes whether its session has
///        keyboard focus (context::has_keyboard_focus()); an unfocused app
///        does not even register the chord, so it never competes with the
///        focused one.
///
/// Chords with Ctrl/Alt/Super or a function key also fire while a text
/// field is being edited (as Windows accelerators do), except the editing
/// chords text fields use themselves (Ctrl+A/C/V/X/Z/Y, Ctrl+arrows, ...).
///
/// Within one top-level window and frame, only the first caller binding a
/// given chord can fire (ImGui alone would fire every binding sharing the
/// window's focus scope). A bare Enter/Space does not fire while the
/// keyboard has a control focused: that control is activated instead, as in
/// a Windows dialog. A 0 @p chord never fires.
bool chord_pressed(ImGuiKeyChord chord, shortcut_scope scope = shortcut_scope::window, bool app_has_focus = false);

/// @brief chord_pressed() for a shortcut string: one or more
///        parse_key_chord() combinations separated by commas ("5, Num5";
///        "Ctrl+," is the comma key). True when any alternative is pressed;
///        an empty or invalid alternative never fires.
bool shortcut_pressed(std::string_view keys, shortcut_scope scope = shortcut_scope::window, bool app_has_focus = false);

/// @brief True when access-key underlines should be drawn: while Alt is held,
///        while the keyboard is driving navigation, or inside a popup menu
///        (the Windows "hide until Alt/keyboard" default).
bool access_keys_visible();

/// @brief Binds the next ImGui item to @p label's access key: Alt+key in a
///        normal window, the bare key inside a popup menu (as in Windows
///        menus). Pressing it activates the item exactly like Space/Enter
///        on it would (press a button, toggle a checkbox, focus an input,
///        open a menu). No-op without an access key.
void set_next_access_key(const access_key_label& label);

/// @brief Underlines @p label's access-key glyph, given where the label text
///        starts (@p text_pos, top-left), in the current window's draw list
///        or @p draw_list. No-op when the label has no access key or
///        access_keys_visible() is false.
void draw_access_key_underline(ImVec2 text_pos, const access_key_label& label, ImDrawList* draw_list = nullptr);

/// @brief Underlines the access key of the last item when it is a framed
///        button whose label ImGui centers per `ImGuiStyle::ButtonTextAlign`
///        (ImGui::Button()).
void underline_button_label(const access_key_label& label);

/// @brief Underlines the access key of the last item when it is a
///        Checkbox/RadioButton (label right of a frame-height square).
void underline_check_label(const access_key_label& label);

/// @brief Underlines the access key of the last item when ImGui drew its
///        label at the item's right edge (InputText, Slider*, Drag*,
///        InputInt/Float, Combo, ColorEdit).
void underline_trailing_label(const access_key_label& label);

/// @brief Position where the next ImGui::BeginMenu()/MenuItem() call draws
///        its label text, in the current (menu bar or popup) window. Must be
///        called right before that call.
ImVec2 next_menu_label_pos();

} // namespace bdg::wish
