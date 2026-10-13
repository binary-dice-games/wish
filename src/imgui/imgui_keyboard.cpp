// MIT License © 2026 Binary Dice Games
/// @file imgui_keyboard.cpp
/// @brief Implementation of the access-key / shortcut-string helpers.
///
/// Lives in its own translation unit because it reads ImGui's navigation and
/// menu-column state from imgui_internal.h, which imgui_ui_renderer.cpp does
/// not include (same split as imgui_icon_widgets.cpp).
#include "imgui_keyboard.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace bdg::wish {

void enable_keyboard_navigation(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

// ── Access keys ──────────────────────────────────────────────────────────────

namespace {

ImGuiKey key_for_char(char c) {
  if (c >= 'a' && c <= 'z')
    return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
  if (c >= 'A' && c <= 'Z')
    return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
  if (c >= '0' && c <= '9')
    return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
  return ImGuiKey_None;
}

/// True inside a popup menu (BeginMenu/MenuButton/ContextMenu popup), where
/// Windows takes access keys without Alt. A modal dialog is a popup too, but
/// behaves like a normal window.
bool in_popup_menu() {
  const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
  return window && (window->Flags & ImGuiWindowFlags_Popup) && !(window->Flags & ImGuiWindowFlags_Modal);
}

} // namespace

access_key_label parse_access_key(const std::string& label, std::string& storage) {
  access_key_label result;
  result.text = label.c_str();
  const size_t visible_end = std::min(label.find("##"), label.size());
  const size_t first_amp = label.find('&');
  if (first_amp == std::string::npos || first_amp >= visible_end)
    return result;

  storage.clear();
  storage.reserve(label.size());
  for (size_t i = 0; i < visible_end; ++i) {
    const char c = label[i];
    if (c == '&' && i + 1 < visible_end) {
      const char next = label[i + 1];
      if (next == '&') {
        storage += '&';
        ++i;
        continue;
      }
      if (result.key == ImGuiKey_None) {
        if (ImGuiKey key = key_for_char(next); key != ImGuiKey_None) {
          result.key = key;
          result.index = static_cast<int>(storage.size());
          continue; // drop the '&'; the marked char is copied next iteration
        }
      }
    }
    storage += c;
  }
  storage.append(label, visible_end, std::string::npos);
  result.text = storage.c_str();
  return result;
}

bool access_keys_visible() {
  const ImGuiContext& g = *GImGui;
  return g.IO.KeyAlt || (g.NavCursorVisible && g.NavInputSource == ImGuiInputSource_Keyboard) || in_popup_menu();
}

void set_next_access_key(const access_key_label& label) {
  if (!label.has_key())
    return;
  const ImGuiKeyChord chord = in_popup_menu() ? ImGuiKeyChord(label.key) : ImGuiKeyChord(ImGuiMod_Alt | label.key);
  ImGui::SetNextItemShortcut(chord, ImGuiInputFlags_RouteFocused);
}

void draw_access_key_underline(ImVec2 text_pos, const access_key_label& label, ImDrawList* draw_list) {
  if (!label.has_key() || !access_keys_visible())
    return;
  const char* glyph = label.text + label.index;
  const float x0 = text_pos.x + ImGui::CalcTextSize(label.text, glyph).x;
  const float x1 = x0 + ImGui::CalcTextSize(glyph, glyph + 1).x;
  const float thickness = std::max(1.0f, std::floor(ImGui::GetFontSize() / 14.0f));
  const float y = std::floor(text_pos.y + ImGui::GetFontBaked()->Ascent + thickness) + 0.5f;
  ImDrawList* dl = draw_list ? draw_list : ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImGuiCol_Text), thickness);
}

void underline_button_label(const access_key_label& label) {
  if (!label.has_key() || !access_keys_visible())
    return;
  // Mirrors ButtonEx()'s RenderTextClipped(bb.Min + FramePadding,
  // bb.Max - FramePadding, ..., ButtonTextAlign).
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 text_size = ImGui::CalcTextSize(label.text, nullptr, true);
  const ImVec2 item_min = ImGui::GetItemRectMin();
  const ImVec2 item_max = ImGui::GetItemRectMax();
  const ImVec2 min(item_min.x + style.FramePadding.x, item_min.y + style.FramePadding.y);
  const ImVec2 max(item_max.x - style.FramePadding.x, item_max.y - style.FramePadding.y);
  ImVec2 pos = min;
  if (style.ButtonTextAlign.x > 0.0f)
    pos.x = std::max(pos.x, pos.x + (max.x - min.x - text_size.x) * style.ButtonTextAlign.x);
  if (style.ButtonTextAlign.y > 0.0f)
    pos.y = std::max(pos.y, pos.y + (max.y - min.y - text_size.y) * style.ButtonTextAlign.y);
  draw_access_key_underline(pos, label);
}

void underline_check_label(const access_key_label& label) {
  if (!label.has_key() || !access_keys_visible())
    return;
  // Checkbox()/RadioButton(): label at check_bb.Max.x + ItemInnerSpacing.x,
  // check_bb.Min.y + FramePadding.y, with a frame-height square check_bb.
  const ImGuiStyle& style = ImGui::GetStyle();
  const ImVec2 min = ImGui::GetItemRectMin();
  draw_access_key_underline(
      ImVec2(min.x + ImGui::GetFrameHeight() + style.ItemInnerSpacing.x, min.y + style.FramePadding.y), label);
}

void underline_trailing_label(const access_key_label& label) {
  if (!label.has_key() || !access_keys_visible())
    return;
  const float label_w = ImGui::CalcTextSize(label.text, nullptr, true).x;
  draw_access_key_underline(
      ImVec2(ImGui::GetItemRectMax().x - label_w, ImGui::GetItemRectMin().y + ImGui::GetStyle().FramePadding.y), label);
}

ImVec2 next_menu_label_pos() {
  const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
  const ImGuiStyle& style = ImGui::GetStyle();
  ImVec2 pos = window->DC.CursorPos;
  // Mirrors BeginMenuEx()/MenuItemEx() (imgui_widgets.cpp): in a menu bar the
  // label is shifted by half the item spacing; in both layouts it then sits
  // at the menu-column label offset, on the current line's text baseline.
  if (window->DC.LayoutType == ImGuiLayoutType_Horizontal)
    pos.x += IM_TRUNC(style.ItemSpacing.x * 0.5f);
  pos.x += window->DC.MenuColumns.OffsetLabel;
  pos.y += window->DC.CurrLineTextBaseOffset;
  return pos;
}

// ── Shortcut strings ─────────────────────────────────────────────────────────

namespace {

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

ImGuiKeyChord modifier_for(std::string_view token) {
  if (iequals(token, "ctrl") || iequals(token, "control"))
    return ImGuiMod_Ctrl;
  if (iequals(token, "shift"))
    return ImGuiMod_Shift;
  if (iequals(token, "alt"))
    return ImGuiMod_Alt;
  if (iequals(token, "super") || iequals(token, "win") || iequals(token, "cmd"))
    return ImGuiMod_Super;
  return 0;
}

ImGuiKey key_for_token(std::string_view token) {
  if (token.size() == 1) {
    if (ImGuiKey key = key_for_char(token[0]); key != ImGuiKey_None)
      return key;
    switch (token[0]) {
      case '+':
        return ImGuiKey_Equal; // the "+" key is Shift+= on most layouts
      case '-':
        return ImGuiKey_Minus;
      case '=':
        return ImGuiKey_Equal;
      case ',':
        return ImGuiKey_Comma;
      case '.':
        return ImGuiKey_Period;
      case '/':
        return ImGuiKey_Slash;
      case ';':
        return ImGuiKey_Semicolon;
      case '\'':
        return ImGuiKey_Apostrophe;
      case '[':
        return ImGuiKey_LeftBracket;
      case ']':
        return ImGuiKey_RightBracket;
      case '\\':
        return ImGuiKey_Backslash;
      case '`':
        return ImGuiKey_GraveAccent;
      default:
        return ImGuiKey_None;
    }
  }
  if ((token[0] == 'F' || token[0] == 'f') && token.size() <= 3) {
    int n = 0;
    for (size_t i = 1; i < token.size(); ++i) {
      if (token[i] < '0' || token[i] > '9')
        return ImGuiKey_None;
      n = n * 10 + (token[i] - '0');
    }
    if (n >= 1 && n <= 24)
      return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
    return ImGuiKey_None;
  }
  struct named_key {
    const char* name;
    ImGuiKey key;
  };
  static constexpr named_key kNamed[] = {
      {"enter", ImGuiKey_Enter},
      {"return", ImGuiKey_Enter},
      {"esc", ImGuiKey_Escape},
      {"escape", ImGuiKey_Escape},
      {"tab", ImGuiKey_Tab},
      {"space", ImGuiKey_Space},
      {"backspace", ImGuiKey_Backspace},
      {"del", ImGuiKey_Delete},
      {"delete", ImGuiKey_Delete},
      {"ins", ImGuiKey_Insert},
      {"insert", ImGuiKey_Insert},
      {"home", ImGuiKey_Home},
      {"end", ImGuiKey_End},
      {"pgup", ImGuiKey_PageUp},
      {"pageup", ImGuiKey_PageUp},
      {"pgdn", ImGuiKey_PageDown},
      {"pagedown", ImGuiKey_PageDown},
      {"up", ImGuiKey_UpArrow},
      {"down", ImGuiKey_DownArrow},
      {"left", ImGuiKey_LeftArrow},
      {"right", ImGuiKey_RightArrow},
      {"plus", ImGuiKey_Equal},
      {"minus", ImGuiKey_Minus},
  };
  for (const auto& named : kNamed)
    if (iequals(token, named.name))
      return named.key;
  return ImGuiKey_None;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

} // namespace

bool chord_pressed(ImGuiKeyChord chord, ImGuiInputFlags route) {
  if (chord == 0)
    return false;
  // (root window, chord) pairs already bound this frame.
  struct claim {
    ImGuiID root;
    ImGuiKeyChord chord;
  };
  static thread_local std::vector<claim> claims;
  static thread_local int claims_frame = -1;
  if (claims_frame != ImGui::GetFrameCount()) {
    claims.clear();
    claims_frame = ImGui::GetFrameCount();
  }
  const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
  const ImGuiID root = window ? window->RootWindow->ID : 0;
  for (const claim& c : claims)
    if (c.root == root && c.chord == chord)
      return false;
  claims.push_back({root, chord});
  return ImGui::Shortcut(chord, route);
}

bool shortcut_pressed(std::string_view keys, ImGuiInputFlags route) {
  return !keys.empty() && chord_pressed(parse_key_chord(keys), route);
}

ImGuiKeyChord parse_key_chord(std::string_view text) {
  text = trim(text);
  ImGuiKeyChord mods = 0;
  ImGuiKey key = ImGuiKey_None;
  size_t pos = 0;
  while (pos < text.size()) {
    // A token runs up to the next '+'; a '+' where a token should start is
    // itself the token (so "Ctrl++" is Ctrl and the "+" key).
    size_t end = text.find('+', pos);
    if (end == pos)
      end = pos + 1;
    else if (end == std::string_view::npos)
      end = text.size();
    std::string_view token = trim(text.substr(pos, end - pos));
    pos = end;
    if (pos < text.size() && text[pos] == '+' && token != "+")
      ++pos; // separator
    if (token.empty())
      return 0;
    if (ImGuiKeyChord mod = modifier_for(token)) {
      mods |= mod;
      continue;
    }
    if (key != ImGuiKey_None)
      return 0; // more than one non-modifier key
    key = key_for_token(token);
    if (key == ImGuiKey_None)
      return 0;
  }
  if (key == ImGuiKey_None)
    return 0;
  return mods | key;
}

} // namespace bdg::wish
