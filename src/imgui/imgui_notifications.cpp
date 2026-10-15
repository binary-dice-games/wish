// MIT License © 2026 Binary Dice Games
/// @file imgui_notifications.cpp
/// @brief Toast drawing for notification_center (see imgui_notifications.hpp).
#include "imgui_notifications.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace bdg::wish {

namespace detail {

std::vector<std::string> split_key_caps(const std::string& keys) {
  std::vector<std::string> caps;
  std::string cur;
  auto flush = [&] {
    const size_t first = cur.find_first_not_of(' ');
    const size_t last = cur.find_last_not_of(' ');
    if (first != std::string::npos)
      caps.push_back(cur.substr(first, last - first + 1));
    cur.clear();
  };
  for (const char c : keys) {
    if (c != '+') {
      cur += c;
    } else if (cur.find_first_not_of(' ') != std::string::npos) {
      flush(); // a separator
    } else {
      caps.push_back("+"); // nothing before it: the plus key itself ("Ctrl++")
      cur.clear();
    }
  }
  flush();
  return caps;
}

float notification_alpha(float age_s, float remaining_s) {
  constexpr float kFadeIn = 0.2f, kFadeOut = 0.5f;
  float a = std::clamp(age_s / kFadeIn, 0.0f, 1.0f);
  if (remaining_s >= 0.0f)
    a = std::min(a, std::clamp(remaining_s / kFadeOut, 0.0f, 1.0f));
  return a;
}

} // namespace detail

namespace {

// ── Look ─────────────────────────────────────────────────────────────────────

ImU32 rgba(int r, int g, int b, float a) {
  return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

ImU32 accent_of(notification_kind kind, float a) {
  switch (kind) {
    case notification_kind::success:
      return rgba(91, 192, 104, a);
    case notification_kind::warning:
      return rgba(240, 173, 78, a);
    case notification_kind::error:
      return rgba(232, 86, 86, a);
    case notification_kind::info:
    default:
      return rgba(26, 159, 255, a);
  }
}

// The kind's glyph, vector-drawn inside a tinted circle centered at @p c.
void draw_kind_icon(ImDrawList* dl, ImVec2 c, float r, notification_kind kind, float a, float font_size) {
  dl->AddCircleFilled(c, r, accent_of(kind, 0.22f * a), 24);
  const ImU32 col = accent_of(kind, a);
  const float t = std::max(1.5f, r * 0.16f);
  switch (kind) {
    case notification_kind::success:
      dl->AddLine({c.x - r * 0.45f, c.y + r * 0.02f}, {c.x - r * 0.1f, c.y + r * 0.38f}, col, t);
      dl->AddLine({c.x - r * 0.1f, c.y + r * 0.38f}, {c.x + r * 0.48f, c.y - r * 0.35f}, col, t);
      break;
    case notification_kind::error:
      dl->AddLine({c.x - r * 0.35f, c.y - r * 0.35f}, {c.x + r * 0.35f, c.y + r * 0.35f}, col, t);
      dl->AddLine({c.x + r * 0.35f, c.y - r * 0.35f}, {c.x - r * 0.35f, c.y + r * 0.35f}, col, t);
      break;
    case notification_kind::warning:
    case notification_kind::info:
    default: {
      const char* glyph = kind == notification_kind::warning ? "!" : "i";
      const float fs = font_size * 1.1f;
      ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, glyph);
      dl->AddText(ImGui::GetFont(), fs, {c.x - ts.x * 0.5f, c.y - ts.y * 0.5f}, col, glyph);
      break;
    }
  }
}

void draw_key_caps(ImDrawList* dl, ImVec2 pos, const std::vector<std::string>& caps, float fs, float pad, float gap,
                   float a) {
  ImFont* font = ImGui::GetFont();
  const float h = fs + pad * 1.2f;
  float x = pos.x;
  for (size_t i = 0; i < caps.size(); ++i) {
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, caps[i].c_str());
    const ImVec2 mn{x, pos.y}, mx{x + ts.x + pad * 2.0f, pos.y + h};
    dl->AddRectFilled({mn.x, mn.y + 2.0f}, {mx.x, mx.y + 2.0f}, rgba(0, 0, 0, 0.35f * a), 4.0f); // key depth
    dl->AddRectFilled(mn, mx, rgba(62, 70, 86, a), 4.0f);
    dl->AddRect(mn, mx, rgba(120, 130, 150, a), 4.0f);
    dl->AddText(font, fs, {mn.x + pad, mn.y + (h - ts.y) * 0.5f}, rgba(240, 244, 250, a), caps[i].c_str());
    x = mx.x;
    if (i + 1 < caps.size()) {
      const ImVec2 ps = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "+");
      dl->AddText(font, fs, {x + gap, pos.y + (h - ps.y) * 0.5f}, rgba(170, 178, 195, a), "+");
      x += gap * 2.0f + ps.x;
    }
  }
}

// ── Layout ───────────────────────────────────────────────────────────────────

struct toast_layout {
  const active_notification* n{nullptr};
  std::vector<std::string> caps;
  float height{0.0f};
  float body_h{0.0f};
};

} // namespace

bool render_notifications(notification_center& center, notification_corner corner, float bottom_inset) {
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  std::vector<active_notification> items = center.active(now);
  if (items.empty())
    return false;
  if (items.size() > kMaxVisibleNotifications)
    items.erase(items.begin(), items.end() - static_cast<std::ptrdiff_t>(kMaxVisibleNotifications));

  const float fs = ImGui::GetFontSize();
  const float title_fs = fs * 1.05f, body_fs = fs * 0.92f, source_fs = fs * 0.8f;
  const float width = std::floor(fs * 23.0f);
  const float pad = std::floor(fs * 0.85f);
  const float icon_r = std::floor(fs * 0.85f);
  const float text_x = pad + icon_r * 2.0f + pad * 0.8f; // left edge of the text column
  const float text_w = width - text_x - pad * 1.6f; // room for the close cross
  const float spacing = std::floor(fs * 0.6f);
  const float margin = std::floor(fs * 1.25f);
  const float cap_pad = std::floor(fs * 0.4f), cap_gap = std::floor(fs * 0.3f);
  ImFont* font = ImGui::GetFont();

  // Measure every toast first: the window must enclose the whole stack.
  std::vector<toast_layout> layouts;
  layouts.reserve(items.size());
  float total_h = 0.0f;
  for (const auto& it : items) {
    toast_layout l;
    l.n = &it;
    l.caps = detail::split_key_caps(it.content.keys);
    float h = pad + title_fs;
    if (!it.content.body.empty()) {
      l.body_h = font->CalcTextSizeA(body_fs, FLT_MAX, text_w, it.content.body.c_str()).y;
      h += fs * 0.3f + l.body_h;
    }
    if (!l.caps.empty())
      h += fs * 0.55f + body_fs + cap_pad * 1.2f + 2.0f;
    if (!it.content.source.empty())
      h += fs * 0.35f + source_fs;
    h = std::max(h + pad, pad * 2.0f + icon_r * 2.0f);
    l.height = std::floor(h);
    total_h += l.height;
    layouts.push_back(std::move(l));
  }
  total_h += spacing * static_cast<float>(layouts.size() - 1);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool right = corner == notification_corner::top_right || corner == notification_corner::bottom_right;
  const bool bottom = corner == notification_corner::bottom_left || corner == notification_corner::bottom_right;
  const float slide = fs * 2.5f; // horizontal slide-in distance
  ImVec2 win_pos{right ? vp->WorkPos.x + vp->WorkSize.x - margin - width - slide : vp->WorkPos.x + margin,
                 bottom ? vp->WorkPos.y + vp->WorkSize.y - bottom_inset - margin - total_h : vp->WorkPos.y + margin};
  ImGui::SetNextWindowPos(win_pos);
  ImGui::SetNextWindowSize({width + slide, total_h});
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("##wish_notifications", nullptr, kFlags);
  ImGui::PopStyleVar(2);
  ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Bottom corners stack upward with the newest nearest the corner, top
  // corners downward: either way, iterate from the corner outward.
  float y = bottom ? win_pos.y + total_h : win_pos.y;
  for (auto li = layouts.rbegin(); li != layouts.rend(); ++li) {
    const auto& l = *li;
    const auto& n = *l.n;
    const float age = std::chrono::duration<float>(now - n.posted).count();
    const bool sticky = n.expires == clock::time_point::max();
    const float remaining = sticky ? -1.0f : std::chrono::duration<float>(n.expires - now).count();
    const float a = detail::notification_alpha(age, remaining);
    const float ease = 1.0f - std::pow(1.0f - std::clamp(age / 0.25f, 0.0f, 1.0f), 3.0f);
    const float x_off = (1.0f - ease) * slide * (right ? 1.0f : -1.0f);

    const float top = bottom ? y - l.height : y;
    const ImVec2 mn{(right ? win_pos.x + slide : win_pos.x) + x_off, top};
    const ImVec2 mx{mn.x + width, top + l.height};
    y = bottom ? top - spacing : top + l.height + spacing;

    ImGui::SetCursorScreenPos(mn);
    ImGui::PushID(static_cast<int>(n.id));
    const bool clicked = ImGui::InvisibleButton("##toast", {width, l.height});
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    if (clicked)
      center.dismiss(n.id);

    // Card: shadow, body, accent bar on the leading edge, hairline border.
    const float rounding = std::floor(fs * 0.5f);
    dl->AddRectFilled({mn.x + 2.0f, mn.y + 4.0f}, {mx.x + 2.0f, mx.y + 4.0f}, rgba(0, 0, 0, 0.30f * a), rounding);
    dl->AddRectFilled(mn, mx, hovered ? rgba(34, 40, 52, 0.98f * a) : rgba(25, 30, 40, 0.97f * a), rounding);
    dl->AddRectFilled(mn, {mn.x + 4.0f, mx.y}, accent_of(n.content.kind, a), rounding, ImDrawFlags_RoundCornersLeft);
    dl->AddRect(mn, mx, rgba(255, 255, 255, 0.08f * a), rounding);

    draw_kind_icon(dl, {mn.x + pad + icon_r, mn.y + pad + icon_r}, icon_r, n.content.kind, a, fs);

    float ty = mn.y + pad;
    dl->AddText(font, title_fs, {mn.x + text_x, ty}, rgba(245, 247, 250, a), n.content.title.c_str());
    ty += title_fs;
    if (!n.content.body.empty()) {
      ty += fs * 0.3f;
      dl->AddText(font, body_fs, {mn.x + text_x, ty}, rgba(190, 198, 212, a), n.content.body.c_str(), nullptr, text_w);
      ty += l.body_h;
    }
    if (!l.caps.empty()) {
      ty += fs * 0.55f;
      draw_key_caps(dl, {mn.x + text_x, ty}, l.caps, body_fs, cap_pad, cap_gap, a);
      ty += body_fs + cap_pad * 1.2f + 2.0f;
    }
    if (!n.content.source.empty()) {
      ty += fs * 0.35f;
      const std::string from = "From " + n.content.source;
      dl->AddText(font, source_fs, {mn.x + text_x, ty}, rgba(130, 140, 160, a), from.c_str());
    }

    if (hovered) { // close cross
      const float cs = fs * 0.32f;
      const ImVec2 c{mx.x - pad, mn.y + pad + cs * 0.5f};
      dl->AddLine({c.x - cs, c.y - cs}, {c.x + cs, c.y + cs}, rgba(200, 205, 215, a), 1.5f);
      dl->AddLine({c.x + cs, c.y - cs}, {c.x - cs, c.y + cs}, rgba(200, 205, 215, a), 1.5f);
    }

    if (!sticky) { // remaining time
      const float total = std::chrono::duration<float>(n.expires - n.posted).count();
      const float frac = total > 0.0f ? std::clamp(remaining / total, 0.0f, 1.0f) : 0.0f;
      dl->AddRectFilled({mn.x + 4.0f, mx.y - 2.0f}, {mn.x + 4.0f + (width - 4.0f - rounding * 0.5f) * frac, mx.y},
                        accent_of(n.content.kind, 0.55f * a));
    }
  }
  ImGui::End();
  return true;
}

} // namespace bdg::wish
