// MIT License © 2026 Binary Dice Games
/// @file imgui_server_overlay.cpp
/// @brief server_overlay implementation (see imgui_server_overlay.hpp).
#include "imgui_server_overlay.hpp"

#include <imgui/imgui_desktop_shell.hpp>
#include <imgui/imgui_keyboard.hpp>
#include <ui/forms/file_browser_utils.hpp>
#include <ui/ui_element.hpp>

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <thread>
#include <tuple>
#include <utility>

namespace bdg::wish {

namespace st = settings;

// ── Pure helpers ─────────────────────────────────────────────────────────────

namespace detail {

std::vector<std::string> read_log_tail(const std::filesystem::path& path, size_t max_lines) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  constexpr std::streamoff kMaxBytes = 256 * 1024;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  const std::streamoff start = size > kMaxBytes ? size - kMaxBytes : 0;
  in.seekg(start);
  std::string text(static_cast<size_t>(size - start), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<size_t>(in.gcount()));

  std::vector<std::string> lines;
  size_t pos = 0;
  if (start > 0) { // the first line is cut: skip it
    pos = text.find('\n');
    pos = pos == std::string::npos ? text.size() : pos + 1;
  }
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos)
      nl = text.size();
    std::string line = text.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    pos = nl + 1;
  }
  if (lines.size() > max_lines)
    lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(max_lines));
  return lines;
}

std::vector<std::string> list_store_identities(const std::filesystem::path& store_dir,
                                               const std::vector<std::string>& connected) {
  std::set<std::string> ids;
  for (const auto& id : connected) {
    if (is_safe_sandbox_identity(id))
      ids.insert(id);
  }
  std::error_code ec;
  for (std::filesystem::directory_iterator it(store_dir / "users", ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().extension() != ".bison")
      continue;
    std::string id = it->path().stem().string();
    if (is_safe_sandbox_identity(id))
      ids.insert(std::move(id));
  }
  return {ids.begin(), ids.end()};
}

std::optional<std::array<float, 4>> parse_hex_color(const std::string& text) {
  if (text.size() != 7 && text.size() != 9)
    return std::nullopt;
  if (text[0] != '#')
    return std::nullopt;
  std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
  for (size_t i = 1, c = 0; i < text.size(); i += 2, ++c) {
    unsigned v = 0;
    for (size_t k = i; k < i + 2; ++k) {
      const int ch = std::tolower(static_cast<unsigned char>(text[k]));
      if (!std::isxdigit(ch))
        return std::nullopt;
      v = v * 16u + static_cast<unsigned>(std::isdigit(ch) ? ch - '0' : ch - 'a' + 10);
    }
    rgba[c] = static_cast<float>(v) / 255.0f;
  }
  return rgba;
}

std::string to_hex_color(const float rgba[4], bool with_alpha) {
  auto byte = [](float f) { return static_cast<unsigned>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
  char buf[16];
  if (with_alpha)
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]), byte(rgba[3]));
  else
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", byte(rgba[0]), byte(rgba[1]), byte(rgba[2]));
  return buf;
}

} // namespace detail

std::vector<overlay_client_info> collect_overlay_clients(const std::vector<sync_context_ptr>& sessions) {
  std::vector<overlay_client_info> out;
  out.reserve(sessions.size());
  for (const auto& sync_ctx : sessions) {
    auto s = context_rlock{*sync_ctx};
    overlay_client_info c;
    c.session_id = s->session_id;
    c.identity = s->identity;
    c.language = s->language();
    c.sandbox = s->resource_dir;
    c.persistent_sandbox = s->resource_dir_persistent;
    c.settings_admin = s->settings_admin;
    c.has_user_store = s->user_store != nullptr;
    c.connected_at = s->connected_at;
    for (const auto& [key, top] : s->top_level_objects) {
      if (!top)
        continue;
      const bison::key_t cls = top->class_key();
      if (cls != bison::key_t{"Window"} && cls != bison::key_t{"DockSpaceViewport"})
        continue;
      std::string title;
      if (const auto* t = top->findField<std::string>(bison::key_t{"title"}))
        title = *t;
      if (title.empty()) {
        if (const auto* id = top->findField<std::string>(bison::key_t{"id"}))
          title = *id;
      }
      c.windows.push_back(detail::desktop_display_title(title, "Window"));
    }
    std::sort(c.windows.begin(), c.windows.end());
    out.push_back(std::move(c));
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.connected_at < b.connected_at; });
  return out;
}

// ── Look ─────────────────────────────────────────────────────────────────────

namespace {

using clock = std::chrono::steady_clock;

// Steam-overlay-like palette: deep blue-gray glass, one bright accent.
constexpr ImVec4 kText{0.91f, 0.93f, 0.96f, 1.0f};
constexpr ImVec4 kMuted{0.56f, 0.61f, 0.69f, 1.0f};
constexpr ImVec4 kAccent{0.10f, 0.62f, 1.00f, 1.0f};
constexpr ImVec4 kDanger{0.86f, 0.27f, 0.27f, 1.0f};
constexpr ImVec4 kWarn{0.95f, 0.68f, 0.30f, 1.0f};
constexpr ImVec4 kGood{0.36f, 0.75f, 0.41f, 1.0f};
constexpr ImVec4 kPanel{0.09f, 0.11f, 0.16f, 0.94f};
constexpr ImVec4 kPanelHi{0.13f, 0.16f, 0.22f, 1.0f};
constexpr ImVec4 kFrame{0.15f, 0.18f, 0.25f, 1.0f};
constexpr ImVec4 kFrameHi{0.20f, 0.24f, 0.33f, 1.0f};

ImVec4 with_alpha(ImVec4 c, float a) {
  c.w *= a;
  return c;
}

/// Pushes the overlay's colors and metrics for the duration of a scope, so
/// it looks the same whatever theme the client sessions use.
class overlay_style {
 public:
  explicit overlay_style(float fs) {
    color(ImGuiCol_Text, kText);
    color(ImGuiCol_TextDisabled, kMuted);
    color(ImGuiCol_WindowBg, ImVec4{0, 0, 0, 0});
    color(ImGuiCol_ChildBg, kPanel);
    color(ImGuiCol_PopupBg, ImVec4{0.10f, 0.12f, 0.17f, 0.98f});
    color(ImGuiCol_Border, ImVec4{1.0f, 1.0f, 1.0f, 0.07f});
    color(ImGuiCol_FrameBg, kFrame);
    color(ImGuiCol_FrameBgHovered, kFrameHi);
    color(ImGuiCol_FrameBgActive, ImVec4{0.22f, 0.28f, 0.40f, 1.0f});
    color(ImGuiCol_Button, kFrame);
    color(ImGuiCol_ButtonHovered, kFrameHi);
    color(ImGuiCol_ButtonActive, with_alpha(kAccent, 0.8f));
    color(ImGuiCol_Header, ImVec4{0.10f, 0.62f, 1.00f, 0.22f});
    color(ImGuiCol_HeaderHovered, ImVec4{0.10f, 0.62f, 1.00f, 0.30f});
    color(ImGuiCol_HeaderActive, ImVec4{0.10f, 0.62f, 1.00f, 0.40f});
    color(ImGuiCol_CheckMark, kAccent);
    color(ImGuiCol_SliderGrab, kAccent);
    color(ImGuiCol_SliderGrabActive, ImVec4{0.35f, 0.75f, 1.0f, 1.0f});
    color(ImGuiCol_Separator, ImVec4{1.0f, 1.0f, 1.0f, 0.08f});
    color(ImGuiCol_TableHeaderBg, kPanelHi);
    color(ImGuiCol_TableRowBg, ImVec4{0, 0, 0, 0});
    color(ImGuiCol_TableRowBgAlt, ImVec4{1.0f, 1.0f, 1.0f, 0.025f});
    color(ImGuiCol_TableBorderLight, ImVec4{1.0f, 1.0f, 1.0f, 0.05f});
    color(ImGuiCol_TableBorderStrong, ImVec4{1.0f, 1.0f, 1.0f, 0.08f});
    color(ImGuiCol_ScrollbarBg, ImVec4{0, 0, 0, 0});
    color(ImGuiCol_ScrollbarGrab, ImVec4{1.0f, 1.0f, 1.0f, 0.12f});
    color(ImGuiCol_ScrollbarGrabHovered, ImVec4{1.0f, 1.0f, 1.0f, 0.20f});
    color(ImGuiCol_ScrollbarGrabActive, ImVec4{1.0f, 1.0f, 1.0f, 0.28f});
    color(ImGuiCol_TextLink, kAccent);
    color(ImGuiCol_ModalWindowDimBg, ImVec4{0.0f, 0.0f, 0.0f, 0.45f});
    color(ImGuiCol_NavCursor, kAccent);
    var(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    var(ImGuiStyleVar_WindowBorderSize, 0.0f);
    var(ImGuiStyleVar_WindowRounding, 0.0f);
    var(ImGuiStyleVar_ChildRounding, std::floor(fs * 0.6f));
    var(ImGuiStyleVar_ChildBorderSize, 1.0f);
    var(ImGuiStyleVar_PopupRounding, std::floor(fs * 0.45f));
    var(ImGuiStyleVar_PopupBorderSize, 1.0f);
    var(ImGuiStyleVar_FrameRounding, std::floor(fs * 0.35f));
    var(ImGuiStyleVar_GrabRounding, std::floor(fs * 0.35f));
    var(ImGuiStyleVar_FramePadding, ImVec2{std::floor(fs * 0.6f), std::floor(fs * 0.32f)});
    var(ImGuiStyleVar_ItemSpacing, ImVec2{std::floor(fs * 0.6f), std::floor(fs * 0.45f)});
    var(ImGuiStyleVar_CellPadding, ImVec2{std::floor(fs * 0.5f), std::floor(fs * 0.3f)});
    var(ImGuiStyleVar_ScrollbarSize, std::floor(fs * 0.7f));
    var(ImGuiStyleVar_FrameBorderSize, 0.0f);
  }
  ~overlay_style() {
    ImGui::PopStyleVar(vars_);
    ImGui::PopStyleColor(colors_);
  }
  overlay_style(const overlay_style&) = delete;
  overlay_style& operator=(const overlay_style&) = delete;

 private:
  void color(ImGuiCol idx, ImVec4 c) {
    ImGui::PushStyleColor(idx, c);
    ++colors_;
  }
  template <typename T>
  void var(ImGuiStyleVar idx, T v) {
    ImGui::PushStyleVar(idx, v);
    ++vars_;
  }
  int colors_{0};
  int vars_{0};
};

/// Text at a size relative to the current font size, for the scope.
class font_scale {
 public:
  explicit font_scale(float factor) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * factor);
  }
  ~font_scale() {
    ImGui::PopFont();
  }
  font_scale(const font_scale&) = delete;
  font_scale& operator=(const font_scale&) = delete;
};

void muted_text(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

void muted_wrapped(const std::string& text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// A small rounded label ("admin", "restart") drawn inline.
void badge(const char* text, ImVec4 color) {
  const float fs = ImGui::GetFontSize() * 0.8f;
  const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
  const ImVec2 pad{fs * 0.45f, fs * 0.12f};
  const ImVec2 size{ts.x + pad.x * 2.0f, ts.y + pad.y * 2.0f};
  ImVec2 pos = ImGui::GetCursorScreenPos();
  pos.y += (ImGui::GetTextLineHeight() - size.y) * 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y}, ImGui::GetColorU32(with_alpha(color, 0.18f)), size.y * 0.5f);
  dl->AddText(ImGui::GetFont(), fs, {pos.x + pad.x, pos.y + pad.y}, ImGui::GetColorU32(color), text);
  ImGui::Dummy({size.x, ImGui::GetTextLineHeight()});
}

// A primary/danger/ghost button with its own colors.
bool styled_button(const char* label, ImVec4 base, ImVec2 size = {0, 0}) {
  ImGui::PushStyleColor(ImGuiCol_Button, base);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4{std::min(base.x + 0.08f, 1.0f), std::min(base.y + 0.08f, 1.0f),
                               std::min(base.z + 0.08f, 1.0f), 1.0f});
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, base);
  const bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return clicked;
}

// An iOS/Steam-style on/off switch. Returns true when toggled.
bool toggle_switch(const char* id, bool* v) {
  const float h = ImGui::GetFrameHeight() * 0.85f;
  const float w = h * 1.8f;
  ImVec2 pos = ImGui::GetCursorScreenPos();
  pos.y += (ImGui::GetFrameHeight() - h) * 0.5f;
  ImGui::SetCursorScreenPos(pos);
  const bool clicked = ImGui::InvisibleButton(id, {w, h});
  if (clicked)
    *v = !*v;
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec4 track = *v ? kAccent : (hovered ? kFrameHi : ImVec4{0.24f, 0.28f, 0.36f, 1.0f});
  dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, ImGui::GetColorU32(track), h * 0.5f);
  const float r = h * 0.5f - 2.5f;
  const float cx = *v ? pos.x + w - h * 0.5f : pos.x + h * 0.5f;
  dl->AddCircleFilled({cx, pos.y + h * 0.5f}, r, IM_COL32(245, 248, 252, 255), 24);
  return clicked;
}

// A rounded card with a title; content goes between begin_card()/end_card().
bool begin_card(const char* id, const char* title, ImVec2 size) {
  const float fs = ImGui::GetFontSize();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 1.1f, fs * 0.9f});
  const bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                                                    (size.y == 0.0f ? ImGuiChildFlags_AutoResizeY : 0));
  ImGui::PopStyleVar();
  if (title && *title) {
    font_scale big{1.1f};
    ImGui::TextUnformatted(title);
    ImGui::Dummy({0.0f, fs * 0.15f});
  }
  return open;
}

void end_card() {
  ImGui::EndChild();
}

// "Label .......... value" rows inside a card.
bool begin_kv(const char* id) {
  if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp))
    return false;
  ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 10.0f);
  ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void kv(const char* key, const std::string& value, const char* tooltip = nullptr) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  muted_text(key);
  ImGui::TableNextColumn();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(value.empty() ? "-" : value.c_str());
  ImGui::PopTextWrapPos();
  if (tooltip && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", tooltip);
}

// A big-number tile ("Uptime  2h 03m").
void stat_tile(const char* id, const char* label, const std::string& value, const std::string& sub, float width,
               ImVec4 accent) {
  const float fs = ImGui::GetFontSize();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 1.0f, fs * 0.75f});
  ImGui::BeginChild(id, {width, 0.0f},
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetWindowPos();
  dl->AddRectFilled({p.x, p.y + fs * 0.9f}, {p.x + 3.0f, p.y + std::max(fs * 0.9f, ImGui::GetWindowSize().y - fs * 0.9f)},
                    ImGui::GetColorU32(accent), 2.0f);
  muted_text(label);
  {
    font_scale big{1.75f};
    ImGui::TextUnformatted(value.c_str());
  }
  if (!sub.empty()) {
    font_scale small{0.85f};
    muted_text(sub.c_str());
  }
  ImGui::EndChild();
}

// Whether a top-level window other than a popup, a tooltip or the
// notification toasts is displayed in front of @p self.
bool client_window_in_front(ImGuiWindow* self) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (int i = g.Windows.Size - 1; i >= 0 && g.Windows[i] != self; --i) {
    const ImGuiWindow* w = g.Windows[i];
    if (!w->WasActive && !w->Active)
      continue;
    if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip))
      continue;
    if (std::strcmp(w->Name, "##wish_notifications") == 0)
      continue;
    return true;
  }
  return false;
}

std::string short_session(bison::key_t id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(id.id & 0xFFFFFFFFu));
  return buf;
}

std::string lower(std::string s) {
  for (auto& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
  return needle.empty() || lower(hay).find(lower(needle)) != std::string::npos;
}

// Built-in settings categories, as the Settings tool shows them.
struct builtin_category {
  const char* id;
  const char* label;
};
constexpr builtin_category kBuiltinCategories[] = {
    {"general", "General"}, {"appearance", "Appearance"}, {"desktop", "Desktop"}, {"server", "Server"}};

std::string capitalize(std::string s) {
  if (!s.empty())
    s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

} // namespace

// ── Settings model ───────────────────────────────────────────────────────────

struct server_overlay::settings_row {
  st::setting_info info;
  st::setting_value stored; ///< monostate when unset (the default applies)
  st::setting_value edit; ///< the editor's current value
  std::string text; ///< text / path editing buffer
  std::string error; ///< last write error
};

struct server_overlay::settings_scope {
  std::string identity; ///< "" = the server store
  persistent_store_ptr store;
  std::vector<st::category_info> categories; ///< with settings, in sidebar order
  std::vector<settings_row> rows; ///< sorted by category, section, order, label
  std::string signature; ///< detects changes between refreshes
};

// ── server_overlay ───────────────────────────────────────────────────────────

server_overlay::server_overlay(server_overlay_info info, notification_center_ptr notifications)
    : info_(std::move(info)), notifications_(std::move(notifications)) {
  hotkey_ = parse_key_chord(info_.hotkey);
}

server_overlay::~server_overlay() = default;

void server_overlay::set_open(bool open) {
  if (open == open_)
    return;
  open_ = open;
  if (open_) {
    focus_next_ = true;
    opened_at_ = clock::now();
    refresh_machine(true);
    log_scroll_to_end_ = true;
  }
}

bool server_overlay::handle_keys() {
  if (hotkey_ != 0 && ImGui::IsKeyChordPressed(hotkey_)) {
    set_open(!open_);
    return true;
  }
  // Escape closes the overlay, unless it is closing one of the overlay's
  // own popups or leaving a text field first.
  if (open_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    set_open(false);
    return true;
  }
  return false;
}

void server_overlay::open_target(const std::string& target, const std::string& what) {
  if (target.empty())
    return;
  if (info_.open_in_shell) {
    if (!info_.open_in_shell(target) && notifications_)
      notifications_->post({"Couldn't open " + what, target, {}, notification_kind::error});
    return;
  }
  // Launching a handler can take a moment (xdg-open waits for its helper):
  // keep the render thread free.
  std::thread([target, what, n = notifications_] {
    if (!open_in_host_shell(target) && n)
      n->post({"Couldn't open " + what, "No application could open " + target, {}, notification_kind::error});
  }).detach();
}

void server_overlay::refresh_machine(bool force) {
  const auto now = clock::now();
  if (!force && now - machine_at_ < std::chrono::seconds{2})
    return;
  machine_at_ = now;
  machine_ = query_machine_info();
}

void server_overlay::render(const std::vector<sync_context_ptr>& sessions) {
  if (!open_)
    return;
  const float fs = ImGui::GetFontSize();
  const auto clients = collect_overlay_clients(sessions);
  refresh_machine(false);

  overlay_style style{fs};
  const float fade = std::clamp(std::chrono::duration<float>(clock::now() - opened_at_).count() / 0.15f, 0.0f, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::SetNextWindowViewport(vp->ID);
  if (focus_next_) {
    ImGui::SetNextWindowFocus();
    focus_next_ = false;
  }
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("##wish_server_overlay", nullptr, kFlags);
  // Always above the client windows, even one that grabbed focus since --
  // but not above the overlay's own popups (combo lists, the shutdown
  // confirmation), so only when a client window is in front of it.
  if (client_window_in_front(ImGui::GetCurrentWindow()))
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

  draw_backdrop();
  draw_top_bar(clients.size());

  // Content: centered column between the top and bottom bars.
  const float top_h = std::floor(fs * 4.2f), bottom_h = std::floor(fs * 4.0f);
  const float max_w = fs * 72.0f;
  const float avail_w = vp->Size.x - fs * 3.0f;
  const float content_w = std::min(max_w, avail_w);
  ImGui::SetCursorScreenPos({vp->Pos.x + (vp->Size.x - content_w) * 0.5f, vp->Pos.y + top_h + fs * 0.6f});
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
  ImGui::BeginChild("##overlay_content", {content_w, vp->Size.y - top_h - bottom_h - fs * 1.2f},
                    ImGuiChildFlags_None, ImGuiWindowFlags_None);
  ImGui::PopStyleColor();
  switch (page_) {
    case page::overview:
      draw_overview(clients);
      break;
    case page::clients:
      draw_clients(clients);
      break;
    case page::settings:
      draw_settings(clients);
      break;
    case page::logs:
      draw_logs();
      break;
    case page::about:
      draw_about();
      break;
  }
  ImGui::EndChild();

  draw_bottom_bar(clients);
  draw_shutdown_popup(clients.size());
  ImGui::End();
  ImGui::PopStyleVar(); // Alpha
}

// ── Chrome ───────────────────────────────────────────────────────────────────

void server_overlay::draw_backdrop() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float a = ImGui::GetStyle().Alpha;
  const ImVec2 mn = vp->Pos, mx{vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y};
  const ImU32 top = ImGui::ColorConvertFloat4ToU32({0.03f, 0.05f, 0.09f, 0.95f * a});
  const ImU32 bottom = ImGui::ColorConvertFloat4ToU32({0.06f, 0.10f, 0.17f, 0.97f * a});
  dl->AddRectFilledMultiColor(mn, mx, top, top, bottom, bottom);
  // A soft accent glow along the top edge.
  const float fs = ImGui::GetFontSize();
  const ImU32 glow = ImGui::ColorConvertFloat4ToU32({0.10f, 0.62f, 1.0f, 0.10f * a});
  const ImU32 none = ImGui::ColorConvertFloat4ToU32({0.10f, 0.62f, 1.0f, 0.0f});
  dl->AddRectFilledMultiColor(mn, {mx.x, mn.y + fs * 8.0f}, glow, glow, none, none);
}

void server_overlay::draw_top_bar(size_t client_count) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float fs = ImGui::GetFontSize();
  const float bar_h = std::floor(fs * 4.2f);
  const float pad = std::floor(fs * 1.5f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine({vp->Pos.x, vp->Pos.y + bar_h}, {vp->Pos.x + vp->Size.x, vp->Pos.y + bar_h},
              ImGui::GetColorU32(ImVec4{1.0f, 1.0f, 1.0f, 0.06f}));

  // Brand, left.
  ImGui::SetCursorScreenPos({vp->Pos.x + pad, vp->Pos.y + fs * 0.9f});
  ImGui::BeginGroup();
  {
    font_scale big{1.6f};
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("wish");
    ImGui::PopStyleColor();
  }
  {
    font_scale small{0.85f};
    muted_text(("SERVER  " + info_.version).c_str());
  }
  ImGui::EndGroup();

  // Page tabs, centered.
  struct tab {
    page p;
    std::string label;
  };
  std::vector<tab> tabs = {{page::overview, "Overview"},
                           {page::clients, "Clients (" + std::to_string(client_count) + ")"},
                           {page::settings, "Settings"},
                           {page::logs, "Logs"},
                           {page::about, "About"}};
  if (!info_.stores)
    tabs.erase(std::remove_if(tabs.begin(), tabs.end(), [](const tab& t) { return t.p == page::settings; }),
               tabs.end());
  float tabs_w = 0.0f;
  const float tab_pad = fs * 1.1f, tab_gap = fs * 0.3f;
  {
    font_scale f{1.05f};
    for (const auto& t : tabs)
      tabs_w += ImGui::CalcTextSize(t.label.c_str()).x + tab_pad * 2.0f + tab_gap;
  }
  float x = vp->Pos.x + (vp->Size.x - tabs_w) * 0.5f;
  const float tab_h = fs * 2.3f;
  const float tab_y = vp->Pos.y + (bar_h - tab_h) * 0.5f;
  {
    font_scale f{1.05f};
    for (const auto& t : tabs) {
      const ImVec2 ts = ImGui::CalcTextSize(t.label.c_str());
      const ImVec2 size{ts.x + tab_pad * 2.0f, tab_h};
      ImGui::SetCursorScreenPos({x, tab_y});
      ImGui::PushID(static_cast<int>(t.p));
      if (ImGui::InvisibleButton("##tab", size))
        page_ = t.p;
      const bool hovered = ImGui::IsItemHovered();
      ImGui::PopID();
      const bool active = page_ == t.p;
      if (active || hovered)
        dl->AddRectFilled({x, tab_y}, {x + size.x, tab_y + size.y},
                          ImGui::GetColorU32(active ? ImVec4{1.0f, 1.0f, 1.0f, 0.08f} : ImVec4{1.0f, 1.0f, 1.0f, 0.04f}),
                          size.y * 0.5f);
      if (active)
        dl->AddRectFilled({x + tab_pad, tab_y + size.y - 3.0f}, {x + size.x - tab_pad, tab_y + size.y - 1.0f},
                          ImGui::GetColorU32(kAccent), 1.0f);
      dl->AddText({x + tab_pad, tab_y + (size.y - ts.y) * 0.5f}, ImGui::GetColorU32(active ? kText : kMuted),
                  t.label.c_str());
      x += size.x + tab_gap;
    }
  }

  // Clock and close button, right.
  const std::time_t now = std::time(nullptr);
  const std::string time_text = detail::desktop_format_time("%H:%M", now);
  const std::string date_text = detail::desktop_format_time("%a %d %b", now);
  const float close_sz = fs * 2.0f;
  const float close_x = vp->Pos.x + vp->Size.x - pad - close_sz;
  float clock_w = 0.0f;
  {
    font_scale big{1.6f};
    clock_w = ImGui::CalcTextSize(time_text.c_str()).x;
  }
  ImGui::SetCursorScreenPos({close_x - fs * 1.2f - clock_w, vp->Pos.y + fs * 0.75f});
  ImGui::BeginGroup();
  {
    font_scale big{1.6f};
    ImGui::TextUnformatted(time_text.c_str());
  }
  {
    font_scale small{0.85f};
    muted_text(date_text.c_str());
  }
  ImGui::EndGroup();

  const ImVec2 cpos{close_x, vp->Pos.y + (bar_h - close_sz) * 0.5f};
  ImGui::SetCursorScreenPos(cpos);
  if (ImGui::InvisibleButton("##overlay_close", {close_sz, close_sz}))
    set_open(false);
  const bool hov = ImGui::IsItemHovered();
  if (hov)
    ImGui::SetTooltip("Close (%s or Esc)", info_.hotkey.c_str());
  const ImVec2 c{cpos.x + close_sz * 0.5f, cpos.y + close_sz * 0.5f};
  dl->AddCircleFilled(c, close_sz * 0.5f, ImGui::GetColorU32(hov ? kFrameHi : kFrame), 24);
  const float k = close_sz * 0.18f;
  dl->AddLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, ImGui::GetColorU32(kText), 2.0f);
  dl->AddLine({c.x + k, c.y - k}, {c.x - k, c.y + k}, ImGui::GetColorU32(kText), 2.0f);
}

void server_overlay::draw_bottom_bar(const std::vector<overlay_client_info>& clients) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float fs = ImGui::GetFontSize();
  const float bar_h = std::floor(fs * 4.0f);
  const float pad = std::floor(fs * 1.5f);
  const float y0 = vp->Pos.y + vp->Size.y - bar_h;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled({vp->Pos.x, y0}, {vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y},
                    ImGui::GetColorU32(ImVec4{0.0f, 0.0f, 0.0f, 0.25f}));
  dl->AddLine({vp->Pos.x, y0}, {vp->Pos.x + vp->Size.x, y0}, ImGui::GetColorU32(ImVec4{1.0f, 1.0f, 1.0f, 0.06f}));

  const float btn_h = fs * 2.2f;
  ImGui::SetCursorScreenPos({vp->Pos.x + pad, y0 + (bar_h - btn_h) * 0.5f});
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{fs * 0.9f, (btn_h - fs) * 0.5f});

  // Sandbox: the persistent root, else pick a client's temporary sandbox.
  if (!info_.sandbox_root.empty()) {
    if (ImGui::Button("Open sandbox folder"))
      open_target(info_.sandbox_root.string(), "the sandbox folder");
    ImGui::SetItemTooltip("%s", info_.sandbox_root.string().c_str());
  } else {
    ImGui::BeginDisabled(clients.empty());
    if (ImGui::Button("Open sandbox folder"))
      ImGui::OpenPopup("##sandbox_pick");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(clients.empty() ? "No client connected; each client gets a temporary sandbox "
                                            "(start the server with --sandbox_root to keep them)"
                                          : "Each client has its own temporary sandbox");
    if (ImGui::BeginPopup("##sandbox_pick")) {
      for (const auto& c : clients) {
        const std::string label = (c.identity.empty() ? std::string{"anonymous"} : c.identity) + "  (" +
            short_session(c.session_id) + ")##" + short_session(c.session_id);
        if (ImGui::MenuItem(label.c_str()))
          open_target(c.sandbox.string(), "the sandbox folder");
        ImGui::SetItemTooltip("%s", c.sandbox.string().c_str());
      }
      ImGui::EndPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Open server log"))
    open_target(info_.log_file.string(), "the server log");
  ImGui::SetItemTooltip("%s", info_.log_file.string().c_str());
  ImGui::SameLine();
  if (ImGui::Button("Logs folder"))
    open_target(info_.log_file.parent_path().string(), "the logs folder");
  ImGui::SameLine(0.0f, fs * 1.5f);
  if (ImGui::Button("GitHub"))
    open_target(kWishGithubUrl, "the project page");
  ImGui::SetItemTooltip("%s", kWishGithubUrl);
  ImGui::SameLine();
  if (ImGui::Button("Documentation"))
    open_target(kWishDocsUrl, "the documentation");
  ImGui::SetItemTooltip("%s", kWishDocsUrl);

  // Right side: hint and shutdown.
  const std::string hint = info_.hotkey.empty() ? "Esc to close" : info_.hotkey + " or Esc to close";
  const char* shutdown_label = "Shut down server";
  const float shutdown_w = ImGui::CalcTextSize(shutdown_label).x + fs * 1.8f;
  const float hint_w = ImGui::CalcTextSize(hint.c_str()).x;
  const float right_x = vp->Pos.x + vp->Size.x - pad;
  if (info_.shutdown) {
    ImGui::SetCursorScreenPos({right_x - shutdown_w, y0 + (bar_h - btn_h) * 0.5f});
    if (styled_button(shutdown_label, ImVec4{0.55f, 0.16f, 0.16f, 1.0f}, {shutdown_w, 0.0f}))
      request_shutdown_popup_ = true;
  }
  const float hint_right = info_.shutdown ? right_x - shutdown_w - fs * 1.2f : right_x;
  const float hint_x = hint_right - hint_w;
  // Only when it doesn't collide with the action buttons.
  if (hint_x > ImGui::GetItemRectMax().x + fs) {
    ImGui::SetCursorScreenPos({hint_x, y0 + (bar_h - fs) * 0.5f});
    muted_text(hint.c_str());
  }
  ImGui::PopStyleVar();
}

void server_overlay::draw_shutdown_popup(size_t client_count) {
  if (request_shutdown_popup_) {
    ImGui::OpenPopup("Shut down server?");
    request_shutdown_popup_ = false;
  }
  const float fs = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 1.4f, fs * 1.2f});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, fs * 0.6f);
  if (ImGui::BeginPopupModal("Shut down server?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoSavedSettings)) {
    {
      font_scale big{1.25f};
      ImGui::TextUnformatted("Shut down the server?");
    }
    ImGui::Dummy({fs * 22.0f, fs * 0.2f});
    if (client_count == 0)
      muted_text("No client is connected.");
    else
      muted_wrapped(std::to_string(client_count) + (client_count == 1 ? " client is" : " clients are") +
                    " connected and will be disconnected.");
    ImGui::Dummy({0.0f, fs * 0.6f});
    if (styled_button("Shut down", ImVec4{0.70f, 0.18f, 0.18f, 1.0f}, {fs * 8.0f, 0.0f})) {
      ImGui::CloseCurrentPopup();
      if (info_.shutdown)
        info_.shutdown();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", {fs * 8.0f, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
}

// ── Overview ─────────────────────────────────────────────────────────────────

void server_overlay::draw_overview(const std::vector<overlay_client_info>& clients) {
  const float fs = ImGui::GetFontSize();
  const float w = ImGui::GetContentRegionAvail().x;
  const float gap = ImGui::GetStyle().ItemSpacing.x;

  size_t window_count = 0;
  for (const auto& c : clients)
    window_count += c.windows.size();
  const auto uptime =
      std::chrono::duration_cast<std::chrono::seconds>(clock::now() - started_at_);

  // Tiles.
  const int tiles = w > fs * 40.0f ? 4 : 2;
  const float tile_w = std::floor((w - gap * static_cast<float>(tiles - 1)) / static_cast<float>(tiles));
  stat_tile("##t_uptime", "Uptime", format_duration(uptime), "since the server started", tile_w, kAccent);
  ImGui::SameLine();
  stat_tile("##t_clients", "Clients", std::to_string(clients.size()),
            std::to_string(window_count) + (window_count == 1 ? " window" : " windows"), tile_w, kGood);
  if (tiles == 4)
    ImGui::SameLine();
  stat_tile("##t_mem", "Server memory", machine_.process_memory ? format_bytes(machine_.process_memory) : "-",
            "resident", tile_w, kWarn);
  ImGui::SameLine();
  char fps[32];
  std::snprintf(fps, sizeof(fps), "%.0f", ImGui::GetIO().Framerate);
  stat_tile("##t_fps", "Frame rate", fps, "frames per second", tile_w, ImVec4{0.68f, 0.45f, 0.95f, 1.0f});

  ImGui::Dummy({0.0f, fs * 0.2f});
  const bool two_cols = w > fs * 46.0f;
  const float card_w = two_cols ? std::floor((w - gap) * 0.5f) : w;

  if (begin_card("##server_card", "Server", {card_w, 0.0f})) {
    if (begin_kv("##server_kv")) {
      kv("Version", info_.version);
      kv("Endpoint", info_.endpoint, "Where clients connect");
      kv("Renderer", info_.renderer);
      kv("Overlay hotkey", info_.hotkey.empty() ? std::string{"(disabled)"} : info_.hotkey);
      kv("Log file", info_.log_file.string());
      kv("Store directory", info_.stores ? info_.stores->dir().string() : std::string{});
      kv("Sandboxes",
         info_.sandbox_root.empty() ? std::string{"temporary, one per client"} : info_.sandbox_root.string());
      std::string admins;
      for (const auto& a : info_.settings_admins)
        admins += (admins.empty() ? "" : ", ") + a;
      kv("Settings admins", admins.empty() ? std::string{"none"} : admins);
      ImGui::EndTable();
    }
  }
  end_card();
  if (two_cols)
    ImGui::SameLine();

  if (begin_card("##machine_card", "This machine", {card_w, 0.0f})) {
    if (begin_kv("##machine_kv")) {
      kv("Host name", machine_.hostname);
      kv("Operating system", machine_.os);
      kv("User", machine_.user);
      kv("CPUs", machine_.cpu_count ? std::to_string(machine_.cpu_count) + " logical" : std::string{});
      if (machine_.load_average) {
        char load[32];
        std::snprintf(load, sizeof(load), "%.2f", *machine_.load_average);
        kv("Load (1 min)", load);
      }
      kv("Process id", std::to_string(machine_.pid));
      ImGui::EndTable();
    }
    if (machine_.total_memory > 0) {
      const uint64_t used = machine_.total_memory - std::min(machine_.available_memory, machine_.total_memory);
      const float frac = static_cast<float>(static_cast<double>(used) / static_cast<double>(machine_.total_memory));
      muted_text("Memory");
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, frac > 0.9f ? kDanger : (frac > 0.75f ? kWarn : kAccent));
      const std::string overlay_text = format_bytes(used) + " of " + format_bytes(machine_.total_memory) + " used";
      ImGui::ProgressBar(frac, {-FLT_MIN, fs * 1.3f}, overlay_text.c_str());
      ImGui::PopStyleColor();
    }
  }
  end_card();

  // Most recent clients, with a link to the full list.
  ImGui::Dummy({0.0f, fs * 0.2f});
  if (begin_card("##recent_card", "Connected clients", {w, 0.0f})) {
    if (clients.empty()) {
      muted_wrapped("No client is connected. Start one with `wish client --run <app>`, or any app "
                    "using a wish client library.");
    } else {
      const size_t shown = std::min<size_t>(clients.size(), 5);
      for (size_t i = clients.size() - shown; i < clients.size(); ++i) {
        const auto& c = clients[i];
        ImGui::TextUnformatted(c.identity.empty() ? "anonymous" : c.identity.c_str());
        ImGui::SameLine();
        std::string windows;
        for (const auto& t : c.windows)
          windows += (windows.empty() ? "" : ", ") + t;
        muted_text(windows.empty() ? "(no windows)" : windows.c_str());
      }
      if (ImGui::TextLink("See all clients"))
        page_ = page::clients;
    }
  }
  end_card();
}

// ── Clients ──────────────────────────────────────────────────────────────────

void server_overlay::draw_clients(const std::vector<overlay_client_info>& clients) {
  const float fs = ImGui::GetFontSize();
  {
    font_scale big{1.4f};
    ImGui::TextUnformatted("Connected clients");
  }
  muted_text(clients.empty() ? "No client is connected."
                             : (std::to_string(clients.size()) + (clients.size() == 1 ? " session" : " sessions") +
                                ", each with its own windows, sandbox and settings.")
                                   .c_str());
  ImGui::Dummy({0.0f, fs * 0.3f});
  if (clients.empty()) {
    if (begin_card("##no_clients", "", {ImGui::GetContentRegionAvail().x, 0.0f})) {
      muted_wrapped("Clients appear here as they connect: `wish client --run <app>`, apps started from "
                    "`wish desktop`, or any program using a wish client library or binding.");
    }
    end_card();
    return;
  }

  const auto now = std::chrono::system_clock::now();
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{fs * 0.7f, fs * 0.55f});
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
  if (ImGui::BeginTable("##clients", 6, kFlags, ImGui::GetContentRegionAvail())) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Session", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Language", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableSetupColumn("Windows", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("Connected", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Sandbox", ImGuiTableColumnFlags_WidthFixed, fs * 6.5f);
    ImGui::TableHeadersRow();
    for (const auto& c : clients) {
      ImGui::PushID(static_cast<int>(c.session_id.id & 0x7FFFFFFF));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (c.identity.empty()) {
        muted_text("anonymous");
      } else {
        ImGui::TextUnformatted(c.identity.c_str());
      }
      if (c.settings_admin) {
        ImGui::SameLine();
        badge("admin", kAccent);
      }
      if (c.has_user_store) {
        ImGui::SameLine();
        badge("store", kGood);
        ImGui::SetItemTooltip("Has a user store: its settings are on the Settings page");
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(short_session(c.session_id).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(c.language.empty() ? "en" : c.language.c_str());
      ImGui::TableNextColumn();
      std::string windows;
      for (const auto& t : c.windows)
        windows += (windows.empty() ? "" : ", ") + t;
      ImGui::PushTextWrapPos(0.0f);
      if (windows.empty())
        muted_text("(no windows)");
      else
        ImGui::TextUnformatted(windows.c_str());
      ImGui::PopTextWrapPos();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(
          format_duration(std::chrono::duration_cast<std::chrono::seconds>(now - c.connected_at)).c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Open"))
        open_target(c.sandbox.string(), "the sandbox folder");
      ImGui::SetItemTooltip("%s%s", c.sandbox.string().c_str(),
                            c.persistent_sandbox ? "\n(persistent)" : "\n(temporary: deleted on disconnect)");
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

// ── Settings ─────────────────────────────────────────────────────────────────

void server_overlay::refresh_settings(const std::vector<overlay_client_info>& clients, bool force) {
  if (!info_.stores)
    return;
  const auto now = clock::now();
  const bool scope_changed = !scope_ || scope_->identity != settings_identity_;
  if (!force && !scope_changed && now - settings_at_ < std::chrono::seconds{2})
    return;
  // Never rebuild under an editor the user is still dragging or typing in.
  if (!force && !scope_changed && ImGui::IsAnyItemActive())
    return;
  settings_at_ = now;

  std::vector<std::string> connected;
  for (const auto& c : clients)
    connected.push_back(c.identity);
  known_identities_ = detail::list_store_identities(info_.stores->dir(), connected);

  auto scope = std::make_unique<settings_scope>();
  scope->identity = settings_identity_;
  const bool server = settings_identity_.empty();
  scope->store = server ? info_.stores->server_store() : info_.stores->user_store(settings_identity_);
  if (!scope->store) {
    scope_ = std::move(scope);
    return;
  }
  st::catalog cat = st::collect(*scope->store, !server);

  std::string sig;
  for (const auto& c : cat.categories)
    sig += st::to_json(c) + "\n";
  for (const auto& d : cat.settings)
    sig += st::to_json(d.info) + "=" + st::to_text(d.value) + "\n";
  if (!force && !scope_changed && scope_ && sig == scope_->signature)
    return; // unchanged: keep the editors' state (errors, half-typed text)
  scope->signature = std::move(sig);

  // Sidebar categories: the built-in ones first, then the declared ones;
  // only those with settings.
  std::set<std::string> used;
  for (auto& d : cat.settings) {
    if (d.info.category.empty())
      d.info.category = "general";
    used.insert(d.info.category);
  }
  std::set<std::string> listed;
  for (const auto& b : kBuiltinCategories) {
    if (used.count(b.id)) {
      st::category_info ci;
      ci.id = b.id;
      ci.label = b.label;
      for (const auto& c : cat.categories) { // a declared description wins
        if (c.id == ci.id && !c.description.empty())
          ci.description = c.description;
      }
      scope->categories.push_back(std::move(ci));
      listed.insert(b.id);
    }
  }
  std::vector<st::category_info> declared;
  for (auto c : cat.categories) {
    if (!used.count(c.id) || !listed.insert(c.id).second)
      continue;
    if (c.label.empty())
      c.label = capitalize(c.id);
    declared.push_back(std::move(c));
  }
  for (const auto& id : used) {
    if (listed.insert(id).second) {
      st::category_info ci;
      ci.id = id;
      ci.label = capitalize(id);
      declared.push_back(std::move(ci));
    }
  }
  std::stable_sort(declared.begin(), declared.end(), [](const auto& a, const auto& b) {
    return std::make_tuple(a.order, lower(a.label)) < std::make_tuple(b.order, lower(b.label));
  });
  for (auto& c : declared)
    scope->categories.push_back(std::move(c));

  for (auto& d : cat.settings) {
    settings_row row;
    row.info = std::move(d.info);
    row.stored = d.value;
    row.edit = std::holds_alternative<std::monostate>(row.stored) ? row.info.default_value : row.stored;
    if (const auto* s = std::get_if<std::string>(&row.edit))
      row.text = *s;
    scope->rows.push_back(std::move(row));
  }
  // Keep the errors of the previous refresh (the value they refer to wasn't saved).
  if (scope_ && !scope_changed) {
    for (auto& row : scope->rows) {
      for (const auto& old : scope_->rows) {
        if (old.info.name == row.info.name && !old.error.empty()) {
          row.error = old.error;
          row.edit = old.edit;
          row.text = old.text;
        }
      }
    }
  }
  if (std::none_of(scope->categories.begin(), scope->categories.end(),
                   [&](const auto& c) { return c.id == settings_category_; }))
    settings_category_ = scope->categories.empty() ? std::string{} : scope->categories.front().id;
  scope_ = std::move(scope);
}

bool server_overlay::draw_setting_editor(settings_row& row) {
  bool commit = false;
  ImGui::SetNextItemWidth(-FLT_MIN);
  switch (row.info.type) {
    case st::setting_type::boolean: {
      bool v = std::holds_alternative<bool>(row.edit) && std::get<bool>(row.edit);
      if (toggle_switch("##v", &v)) {
        row.edit = v;
        commit = true;
      }
      break;
    }
    case st::setting_type::integer: {
      int v = 0;
      if (const auto* i = std::get_if<int32_t>(&row.edit))
        v = *i;
      else if (const auto* f = std::get_if<float>(&row.edit))
        v = static_cast<int>(*f);
      if (row.info.min && row.info.max) {
        ImGui::SliderInt("##v", &v, static_cast<int>(*row.info.min), static_cast<int>(*row.info.max), "%d",
                         ImGuiSliderFlags_AlwaysClamp);
      } else {
        ImGui::InputInt("##v", &v);
      }
      row.edit = static_cast<int32_t>(v);
      commit = ImGui::IsItemDeactivatedAfterEdit();
      break;
    }
    case st::setting_type::real: {
      float v = 0.0f;
      if (const auto* f = std::get_if<float>(&row.edit))
        v = *f;
      else if (const auto* i = std::get_if<int32_t>(&row.edit))
        v = static_cast<float>(*i);
      if (row.info.min && row.info.max)
        ImGui::SliderFloat("##v", &v, *row.info.min, *row.info.max, "%.2f", ImGuiSliderFlags_AlwaysClamp);
      else
        ImGui::InputFloat("##v", &v, row.info.step.value_or(0.0f), 0.0f, "%.3f");
      row.edit = v;
      commit = ImGui::IsItemDeactivatedAfterEdit();
      break;
    }
    case st::setting_type::choice: {
      const std::string current = std::holds_alternative<std::string>(row.edit) ? std::get<std::string>(row.edit) : "";
      std::string preview = current;
      for (const auto& o : row.info.options) {
        if (o.value == current && !o.label.empty())
          preview = o.label;
      }
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (const auto& o : row.info.options) {
          const bool selected = o.value == current;
          if (ImGui::Selectable(o.label.empty() ? o.value.c_str() : o.label.c_str(), selected)) {
            row.edit = o.value;
            commit = true;
          }
          if (selected)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      break;
    }
    case st::setting_type::color: {
      const std::string current = std::holds_alternative<std::string>(row.edit) ? std::get<std::string>(row.edit) : "";
      const bool alpha = current.size() == 9;
      auto rgba = detail::parse_hex_color(current).value_or(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
      ImGuiColorEditFlags flags = ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_InputRGB;
      if (!alpha)
        flags |= ImGuiColorEditFlags_NoAlpha;
      if (ImGui::ColorEdit4("##v", rgba.data(), flags))
        row.edit = detail::to_hex_color(rgba.data(), alpha);
      commit = ImGui::IsItemDeactivatedAfterEdit();
      break;
    }
    case st::setting_type::text:
    case st::setting_type::path:
    default: {
      char buf[1024];
      std::snprintf(buf, sizeof(buf), "%s", row.text.c_str());
      if (ImGui::InputTextWithHint("##v", row.info.type == st::setting_type::path ? "path" : "", buf, sizeof(buf)))
        row.text = buf;
      row.edit = row.text;
      commit = ImGui::IsItemDeactivatedAfterEdit();
      break;
    }
  }
  return commit;
}

void server_overlay::draw_settings(const std::vector<overlay_client_info>& clients) {
  refresh_settings(clients, false);
  const float fs = ImGui::GetFontSize();
  const float side_w = std::floor(fs * 14.0f);
  const float h = ImGui::GetContentRegionAvail().y;

  // Sidebar: stores, then the selected store's categories.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 0.6f, fs * 0.8f});
  ImGui::BeginChild("##settings_side", {side_w, h}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleVar();
  {
    font_scale small{0.8f};
    muted_text("  STORE");
  }
  if (ImGui::Selectable("  Server", settings_identity_.empty()))
    settings_identity_.clear();
  ImGui::SetItemTooltip("Server store: server-wide defaults (e.g. the saved `wish server` flags)");
  for (const auto& id : known_identities_) {
    const std::string label = "  " + id + "##user_" + id;
    if (ImGui::Selectable(label.c_str(), settings_identity_ == id))
      settings_identity_ = id;
    ImGui::SetItemTooltip("User store of '%s': the apps' settings for this user", id.c_str());
  }
  if (scope_ && !scope_->categories.empty()) {
    ImGui::Dummy({0.0f, fs * 0.4f});
    {
      font_scale small{0.8f};
      muted_text("  CATEGORY");
    }
    for (const auto& c : scope_->categories) {
      const std::string label = "  " + c.label + "##cat_" + c.id;
      if (ImGui::Selectable(label.c_str(), settings_filter_.empty() && settings_category_ == c.id)) {
        settings_category_ = c.id;
        settings_filter_.clear();
      }
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // Panel.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0, 0, 0, 0});
  ImGui::BeginChild("##settings_panel", {0.0f, h});
  ImGui::PopStyleColor();
  if (!scope_ || !scope_->store) {
    muted_wrapped("This store can't be opened.");
    ImGui::EndChild();
    return;
  }
  const bool server = scope_->identity.empty();
  {
    font_scale big{1.4f};
    ImGui::TextUnformatted(server ? "Server settings" : (scope_->identity + "'s app settings").c_str());
  }
  muted_wrapped(server ? "Saved in the server store. Most take effect the next time the server starts."
                       : "Saved in this user's store; apps pick them up as they read their settings.");
  ImGui::SetNextItemWidth(std::min(fs * 22.0f, ImGui::GetContentRegionAvail().x));
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", settings_filter_.c_str());
    if (ImGui::InputTextWithHint("##filter", "Search settings", buf, sizeof(buf)))
      settings_filter_ = buf;
  }
  ImGui::Dummy({0.0f, fs * 0.2f});

  if (scope_->rows.empty()) {
    muted_wrapped(server ? "No server setting is described yet."
                         : "No setting is described in this user's store yet. Apps describe theirs as they run "
                           "(see docs/settings.md).");
    ImGui::EndChild();
    return;
  }

  // Rows to show: the category's, or every match of the search.
  const bool searching = !settings_filter_.empty();
  std::map<std::string, std::vector<settings_row*>> by_section; // "<category>\x1f<section>"
  std::vector<std::string> section_order;
  for (auto& row : scope_->rows) {
    if (searching) {
      if (!contains_ci(row.info.label, settings_filter_) && !contains_ci(row.info.description, settings_filter_) &&
          !contains_ci(row.info.name, settings_filter_))
        continue;
    } else if (row.info.category != settings_category_) {
      continue;
    }
    const std::string key = row.info.category + '\x1f' + row.info.section;
    if (!by_section.count(key))
      section_order.push_back(key);
    by_section[key].push_back(&row);
  }
  if (section_order.empty())
    muted_text("No setting matches.");

  const float editor_w = std::floor(fs * 15.0f);
  int card_index = 0;
  for (const auto& key : section_order) {
    const std::string cat_id = key.substr(0, key.find('\x1f'));
    const std::string section = key.substr(key.find('\x1f') + 1);
    std::string title = section;
    if (searching) {
      for (const auto& c : scope_->categories) {
        if (c.id == cat_id)
          title = c.label + (section.empty() ? "" : "  /  " + section);
      }
    }
    ImGui::PushID(card_index++);
    if (begin_card("##section", title.c_str(), {ImGui::GetContentRegionAvail().x, 0.0f})) {
      if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("editor", ImGuiTableColumnFlags_WidthFixed, editor_w + fs * 4.5f);
        for (auto* row : by_section[key]) {
          ImGui::PushID(row->info.name.c_str());
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(row->info.label.empty() ? row->info.name.c_str() : row->info.label.c_str());
          if (row->info.restart) {
            ImGui::SameLine();
            badge("restart", kWarn);
            ImGui::SetItemTooltip("Takes effect after a restart");
          }
          if (!row->info.description.empty())
            muted_wrapped(row->info.description);
          if (!row->error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
            ImGui::TextWrapped("%s", row->error.c_str());
            ImGui::PopStyleColor();
          }
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(editor_w);
          ImGui::BeginGroup();
          ImGui::PushItemWidth(editor_w);
          const bool commit = draw_setting_editor(*row);
          ImGui::PopItemWidth();
          ImGui::EndGroup();
          const bool is_default = std::holds_alternative<std::monostate>(row->stored);
          ImGui::SameLine();
          ImGui::BeginDisabled(is_default);
          const bool reset = ImGui::SmallButton("Reset");
          ImGui::EndDisabled();
          ImGui::SetItemTooltip(is_default ? "Default value" : "Reset to the default (%s)",
                                st::to_text(row->info.default_value).c_str());
          if (commit) {
            row->error = st::write_setting(*scope_->store, row->info, row->edit);
            if (row->error.empty())
              settings_at_ = {}; // re-read on the next frame
          }
          if (reset) {
            st::reset_setting(*scope_->store, row->info);
            row->error.clear();
            settings_at_ = {};
          }
          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
    end_card();
    ImGui::PopID();
    ImGui::Dummy({0.0f, fs * 0.15f});
  }
  ImGui::EndChild();
}

// ── Logs ─────────────────────────────────────────────────────────────────────

void server_overlay::refresh_log(bool force) {
  const auto now = clock::now();
  if (!force && now - log_at_ < std::chrono::seconds{1})
    return;
  log_at_ = now;
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(info_.log_file, ec);
  if (!force && !ec && mtime == log_mtime_ && !log_lines_.empty())
    return;
  log_mtime_ = ec ? std::filesystem::file_time_type{} : mtime;
  const size_t before = log_lines_.size();
  log_lines_ = detail::read_log_tail(info_.log_file, 2000);
  if (log_follow_ && log_lines_.size() != before)
    log_scroll_to_end_ = true;
}

void server_overlay::draw_logs() {
  refresh_log(false);
  const float fs = ImGui::GetFontSize();
  {
    font_scale big{1.4f};
    ImGui::TextUnformatted("Server log");
  }
  muted_text(info_.log_file.string().c_str());
  ImGui::SetNextItemWidth(fs * 18.0f);
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", log_filter_.c_str());
    if (ImGui::InputTextWithHint("##log_filter", "Filter", buf, sizeof(buf)))
      log_filter_ = buf;
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Follow", &log_follow_) && log_follow_)
    log_scroll_to_end_ = true;
  ImGui::SameLine();
  if (ImGui::Button("Reload"))
    refresh_log(true);
  ImGui::SameLine();
  if (ImGui::Button("Copy")) {
    std::string all;
    for (const auto& l : log_lines_) {
      if (contains_ci(l, log_filter_))
        all += l + "\n";
    }
    ImGui::SetClipboardText(all.c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Open file"))
    open_target(info_.log_file.string(), "the server log");

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{fs * 0.8f, fs * 0.6f});
  ImGui::BeginChild("##log_view", {0.0f, 0.0f}, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PopStyleVar();
  if (log_lines_.empty()) {
    muted_wrapped("The log is empty. Start the server with --verbose info (or trace) to log connections and "
                  "RMI calls; apps' own log messages appear at --verbose warning and above.");
  } else {
    std::vector<const std::string*> shown;
    shown.reserve(log_lines_.size());
    for (const auto& l : log_lines_) {
      if (contains_ci(l, log_filter_))
        shown.push_back(&l);
    }
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const std::string& l = *shown[static_cast<size_t>(i)];
        const std::string lo = lower(l);
        ImVec4 col = kText;
        if (lo.find("error") != std::string::npos || lo.find("fatal") != std::string::npos)
          col = kDanger;
        else if (lo.find("warn") != std::string::npos)
          col = kWarn;
        else if (lo.find("[rmi]") != std::string::npos)
          col = kMuted;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(l.c_str());
        ImGui::PopStyleColor();
      }
    }
    if (log_scroll_to_end_) {
      ImGui::SetScrollHereY(1.0f);
      log_scroll_to_end_ = false;
    }
  }
  ImGui::EndChild();
}

// ── About ────────────────────────────────────────────────────────────────────

void server_overlay::draw_about() {
  const float fs = ImGui::GetFontSize();
  const float w = ImGui::GetContentRegionAvail().x;
  if (begin_card("##about", "", {w, 0.0f})) {
    {
      font_scale big{2.4f};
      ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
      ImGui::TextUnformatted("wish");
      ImGui::PopStyleColor();
    }
    muted_text(("Version " + info_.version + "   -   Dear ImGui " + ImGui::GetVersion()).c_str());
    ImGui::Dummy({0.0f, fs * 0.4f});
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "A remote UI framework: client programs build their user interface on this server, which draws "
        "it and sends their users' input back to them as events.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0.0f, fs * 0.4f});
    if (ImGui::TextLink("Project on GitHub"))
      open_target(kWishGithubUrl, "the project page");
    ImGui::SameLine(0.0f, fs * 1.5f);
    if (ImGui::TextLink("Documentation"))
      open_target(kWishDocsUrl, "the documentation");
    ImGui::SameLine(0.0f, fs * 1.5f);
    if (ImGui::TextLink("Report an issue"))
      open_target(std::string{kWishGithubUrl} + "/issues", "the issue tracker");
    ImGui::Dummy({0.0f, fs * 0.4f});
    muted_text("MIT License. (c) Binary Dice Games.");
  }
  end_card();

  ImGui::Dummy({0.0f, fs * 0.2f});
  if (begin_card("##keys", "Keyboard", {w, 0.0f})) {
    if (begin_kv("##keys_kv")) {
      kv(info_.hotkey.empty() ? "(none)" : info_.hotkey.c_str(), "Open or close this overlay");
      kv("Esc", "Close this overlay");
      kv("Tab / Shift+Tab", "Move between controls");
      ImGui::EndTable();
    }
  }
  end_card();
}

} // namespace bdg::wish
