// MIT License © 2026 Binary Dice Games
/// @file imgui_desktop_shell.cpp
/// @brief Desktop chrome drawn for a session's `DesktopShell` top-level
///        object -- see imgui_desktop_shell.hpp.
#include "imgui_desktop_shell.hpp"

#include <context/file_service.hpp>
#include <context/style_service.hpp>
#include <ui/ui_element.hpp>
#include <ui/ui_elements/window.hpp>
#include "imgui_icon_widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

namespace bdg::wish {

using namespace bison;

namespace detail {

std::string desktop_display_title(const std::string& title, const std::string& fallback) {
  std::string shown = title.substr(0, title.find("##"));
  return shown.empty() ? fallback : shown;
}

std::string desktop_format_time(const std::string& format, std::time_t t) {
  if (format.empty())
    return {};
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &t);
#else
  localtime_r(&t, &local);
#endif
  std::array<char, 128> buf{};
  size_t n = std::strftime(buf.data(), buf.size(), format.c_str(), &local);
  return std::string(buf.data(), n);
}

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

} // namespace

int desktop_app_match_rank(const desktop_app_info& app, const std::string& query) {
  if (query.empty())
    return 0;
  std::string q = to_lower(query);
  std::string label = to_lower(app.label);
  auto at = label.find(q);
  if (at == 0)
    return 0;
  if (at != std::string::npos)
    return 1;
  if (to_lower(app.description).find(q) != std::string::npos)
    return 2;
  return -1;
}

bool desktop_app_matches(const desktop_app_info& app, const std::string& query) {
  return desktop_app_match_rank(app, query) >= 0;
}

std::vector<desktop_app_info> desktop_collect_apps(const ui_element& shell) {
  std::vector<desktop_app_info> apps;
  shell.for_each_child_ordered([&](key_t, const ui_element& child) {
    if (child.class_key() != key_t{"DesktopApp"})
      return;
    desktop_app_info a;
    a.node_id = child.wish_id();
    a.app = child.get_as<std::string>("app"_key, std::string{});
    a.label = child.get_as<std::string>("label"_key, std::string{});
    a.description = child.get_as<std::string>("description"_key, std::string{});
    a.collection = child.get_as<std::string>("collection"_key, std::string{});
    a.icon = child.get_as<std::string>("icon"_key, std::string{});
    a.pinned = child.get_as<bool>("pinned"_key, false);
    a.on_desktop = child.get_as<bool>("on_desktop"_key, false);
    if (const field* f = child.findField("sessions"_key); f && f->is<std::vector<int32_t>>())
      a.sessions = f->as<std::vector<int32_t>>();
    if (a.label.empty())
      a.label = a.app;
    apps.push_back(std::move(a));
  });
  return apps;
}

std::vector<desktop_window_info>
desktop_collect_windows(const context& s, const sync_context* session, const std::vector<desktop_app_info>& apps) {
  // The object group (bridge client) each object was created under; 0 for
  // none.
  auto group_of = [&](hash_t id) -> hash_t {
    for (const auto& [group, ids] : s.groups)
      if (ids.count(id) != 0)
        return group;
    return 0u;
  };
  auto app_of = [&](hash_t group) -> int {
    if (group == 0u)
      return -1;
    for (size_t i = 0; i < apps.size(); ++i)
      for (int32_t g : apps[i].sessions)
        if (static_cast<hash_t>(g) == group)
          return int(i);
    return -1;
  };

  struct top {
    key_t key;
    const ui_element* root;
    hash_t group;
    bool frame;
  };
  std::vector<top> tops;
  std::set<hash_t> framed;
  for (const auto& [key, root] : s.top_level_objects) {
    if (!root || !root->visible())
      continue;
    const key_t cls = root->class_key();
    bool frame = cls == key_t{"DockSpaceViewport"};
    if (!frame && (cls != key_t{"Window"} || static_cast<const window&>(*root).modal(false)))
      continue;
    hash_t group = group_of(root->wish_id().id);
    if (frame && group != 0u)
      framed.insert(group);
    tops.push_back({key, root.get(), group, frame});
  }

  std::vector<desktop_window_info> out;
  for (const auto& t : tops) {
    // A framed app's own windows are reached through its frame.
    if (!t.frame && t.group != 0u && framed.count(t.group) != 0)
      continue;
    desktop_window_info info;
    info.session = session;
    info.top_key = t.key;
    info.frame = t.frame;
    info.app_index = app_of(t.group);
    info.members.push_back(t.key);
    std::string title;
    if (t.frame) {
      const auto& dsv = static_cast<const ui_dockspace_viewport&>(*t.root);
      info.minimized = dsv.minimized();
      info.focused = dsv.is_focused();
      title = dsv.title_ref().empty() ? dsv.id_ref() : dsv.title_ref();
      for (const auto& o : tops) {
        if (o.key == t.key || o.group != t.group || t.group == 0u)
          continue;
        info.members.push_back(o.key);
        if (!o.frame && static_cast<const window&>(*o.root).is_focused())
          info.focused = true;
      }
    } else {
      const auto& win = static_cast<const window&>(*t.root);
      info.minimized = win.minimized();
      info.focused = win.is_focused();
      info.closable = win.closable(false);
      title = win.title_ref();
    }
    std::string fallback = info.app_index >= 0 ? apps[size_t(info.app_index)].label : std::string{"Window"};
    info.title = desktop_display_title(title, fallback);
    out.push_back(std::move(info));
  }
  return out;
}

int desktop_days_in_month(int year, int month) {
  static constexpr std::array<int, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 1 && leap ? 29 : kDays[size_t(month)];
}

int desktop_first_weekday(int year, int month) {
  // Sakamoto's algorithm: 0 = Sunday; shifted so 0 = Monday.
  static constexpr std::array<int, 12> kT{0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  int m = month + 1;
  if (m < 3)
    y -= 1;
  int sunday_based = (y + y / 4 - y / 100 + y / 400 + kT[size_t(m - 1)] + 1) % 7;
  return (sunday_based + 6) % 7;
}

} // namespace detail

namespace {

// ── Frame snapshot / actions ─────────────────────────────────────────────────

constexpr const char* kStartIcon = "res/icons/apps.png";
constexpr const char* kDefaultAppIcon = "res/icons/apps.png";
constexpr const char* kShowDesktopIcon = "res/icons/desktop.png";
constexpr const char* kPinIcon = "res/icons/push_pin.png";
constexpr const char* kPowerIcon = "res/icons/power.png";
constexpr const char* kUserIcon = "res/icons/person.png";
constexpr const char* kLightIcon = "res/icons/light_mode.png";
constexpr const char* kDarkIcon = "res/icons/dark_mode.png";
constexpr const char* kClientsIcon = "res/icons/server.png";
constexpr const char* kSearchIcon = "res/icons/search.png";

/// Everything the chrome shows, copied out under the session locks.
struct shell_frame {
  sync_context_ptr shell_session;
  key_t shell_top_key{0u};
  key_t shell_id{0u};
  std::filesystem::path resource_dir;
  bool allow_absolute_paths{false};
  const std::unordered_map<std::string, uint32_t>* embedded_crc32s{nullptr};
  std::string wallpaper;
  std::string user;
  std::string time_format;
  std::string date_format;
  bool show_icons{true};
  int32_t clients{0};
  std::optional<ImGuiStyle> style;
  bool light{false};
  std::vector<desktop_app_info> apps;
  std::vector<desktop_window_info> windows;
};

/// Something the user did this frame, applied under the owning session's
/// write lock after drawing.
struct shell_action {
  const sync_context* session{nullptr};
  std::function<void(context&)> apply;
};

/// Finds the first session with a DesktopShell and copies out everything the
/// chrome needs, locking each session once.
std::optional<shell_frame> snapshot(imgui_renderer& r, const std::vector<sync_context_ptr>& sessions) {
  shell_frame f;
  bool found = false;
  for (const auto& sync_ctx : sessions) {
    auto sess = context_wlock{*sync_ctx};
    const ui_element* shell = nullptr;
    key_t shell_key{0u};
    if (!found) {
      for (const auto& [key, root] : sess->top_level_objects) {
        if (root && root->class_key() == key_t{"DesktopShell"}) {
          shell = root.get();
          shell_key = key;
          break;
        }
      }
    }
    if (shell) {
      found = true;
      f.shell_session = sync_ctx;
      f.shell_top_key = shell_key;
      f.shell_id = shell->wish_id();
      f.resource_dir = sess->resource_dir;
      f.allow_absolute_paths = sess->allow_absolute_paths;
      f.embedded_crc32s = &sess->embedded_crc32s;
      f.wallpaper = shell->get_as<std::string>("wallpaper"_key, std::string{});
      f.user = shell->get_as<std::string>("user"_key, std::string{});
      f.time_format = shell->get_as<std::string>("time_format"_key, std::string{"%H:%M"});
      f.date_format = shell->get_as<std::string>("date_format"_key, std::string{"%d/%m/%Y"});
      f.show_icons = shell->get_as<bool>("show_icons"_key, true);
      f.clients = shell->get_as<int32_t>("clients"_key, 0);
      if (const ImGuiStyle* st = r.session_style(*sess))
        f.style = *st;
      f.light = sess->style_service && sess->style_service->is_light_theme();
      f.apps = detail::desktop_collect_apps(*shell);
      auto wins = detail::desktop_collect_windows(*sess, sync_ctx.get(), f.apps);
      f.windows.insert(f.windows.end(), wins.begin(), wins.end());
    } else {
      auto wins = detail::desktop_collect_windows(*sess, sync_ctx.get(), {});
      f.windows.insert(f.windows.end(), wins.begin(), wins.end());
    }
  }
  if (!found)
    return std::nullopt;
  return f;
}

/// Minimizes or restores the top-level @p top_key of @p s (a Window or a
/// DockSpaceViewport), if it still exists; @p raise also brings it to the
/// front.
void set_frame_state(context& s, key_t top_key, bool minimized, bool raise) {
  auto it = s.top_level_objects.find(top_key);
  if (it == s.top_level_objects.end() || !it->second)
    return;
  const key_t cls = it->second->class_key();
  if (cls == key_t{"Window"}) {
    const auto& w = static_cast<const window&>(*it->second);
    w.set_minimized(minimized);
    if (raise)
      w.request_raise();
  } else if (cls == key_t{"DockSpaceViewport"}) {
    const auto& d = static_cast<const ui_dockspace_viewport&>(*it->second);
    d.set_minimized(minimized);
    if (raise)
      d.request_raise();
  }
}

// ── Drawing context ──────────────────────────────────────────────────────────

class shell_painter {
 public:
  shell_painter(
      imgui_renderer& r,
      shell_frame& f,
      desktop_shell_state& st,
      std::vector<shell_action>& actions,
      const std::function<void()>& shutdown_server)
      : r_(r), f_(f), st_(st), actions_(actions), shutdown_server_(shutdown_server) {
    if (st_.resolved_for_dir != f_.resource_dir.string()) {
      st_.resolved_paths.clear();
      st_.resolved_for_dir = f_.resource_dir.string();
    }
  }

  void draw();

 private:
  // ── Helpers ──

  ImTextureID texture(const std::string& src) {
    const std::string& full = resolved(src);
    if (full.empty())
      return ImTextureID{};
    return r_.get_or_load_texture(full, f_.resource_dir, f_.embedded_crc32s);
  }

  const std::string& resolved(const std::string& src) {
    auto it = st_.resolved_paths.find(src);
    if (it != st_.resolved_paths.end())
      return it->second;
    auto p = file_service::resolve_path(src, f_.resource_dir, f_.allow_absolute_paths);
    return st_.resolved_paths[src] = p.empty() ? std::string{} : p.string();
  }

  ImTextureID app_icon(int app_index) {
    if (app_index >= 0) {
      const auto& icon = f_.apps[size_t(app_index)].icon;
      if (ImTextureID t = icon.empty() ? ImTextureID{} : texture(icon))
        return t;
    }
    return texture(kDefaultAppIcon);
  }

  ImVec4 text_color() const {
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
  }

  /// A flat, square taskbar button: hover/press highlight and a centered
  /// icon of @p icon pixels. Falls back to a text button without a texture.
  bool bar_button(const char* id, ImTextureID tex, float size, float icon, ImU32 tint, const char* fallback) {
    if (!tex)
      return ImGui::Button(fallback, ImVec2(0.0f, size));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive())
      dl->AddRectFilled(p0, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_ButtonActive, 0.7f), 6.0f);
    else if (ImGui::IsItemHovered())
      dl->AddRectFilled(p0, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.6f), 6.0f);
    ImVec2 i0(p0.x + (size - icon) * 0.5f, p0.y + (size - icon) * 0.5f);
    dl->AddImage(ImTextureRef(tex), i0, ImVec2(i0.x + icon, i0.y + icon), ImVec2(0, 0), ImVec2(1, 1), tint);
    return clicked;
  }

  ImU32 accent(float alpha = 1.0f) const {
    ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    c.w *= alpha;
    return ImGui::GetColorU32(c);
  }

  /// Queues @p fn to run on the shell's own session.
  void on_shell(std::function<void(context&)> fn) {
    actions_.push_back({f_.shell_session.get(), std::move(fn)});
  }

  /// Queues a DesktopShell event for the owning client.
  void emit(const char* event, dynamic payload = {}) {
    key_t id = f_.shell_id;
    key_t top = f_.shell_top_key;
    key_t name{event};
    on_shell([id, top, name, payload = std::move(payload)](context& s) mutable {
      key_t saved = s.current_top_level_key;
      s.current_top_level_key = top;
      enqueue_event(s, id, name, std::move(payload));
      s.current_top_level_key = saved;
      s.dirty.store(kDirtySettleFrames, std::memory_order_release);
    });
  }

  void launch(const desktop_app_info& app) {
    dynamic p;
    p["app"_key] = app.app;
    emit("launch", std::move(p));
  }

  void set_pinned(const desktop_app_info& app, bool pinned) {
    dynamic p;
    p["app"_key] = app.app;
    p["pinned"_key] = pinned;
    emit("pin", std::move(p));
  }

  void set_on_desktop(const desktop_app_info& app, bool on_desktop) {
    dynamic p;
    p["app"_key] = app.app;
    p["on_desktop"_key] = on_desktop;
    emit("desktop_icon", std::move(p));
  }

  /// Minimizes, or restores, every member of @p w, raising its main window.
  void set_state(const desktop_window_info& w, bool minimized) {
    std::vector<key_t> keys(w.members.rbegin(), w.members.rend());
    key_t main = w.top_key;
    // Only the entry's own window is raised: raising a window docked in a
    // frame would also switch the frame's active tab.
    actions_.push_back({w.session, [keys, main, minimized](context& s) {
                          for (key_t k : keys)
                            set_frame_state(s, k, minimized, !minimized && k == main);
                          s.dirty.store(kDirtySettleFrames, std::memory_order_release);
                        }});
  }

  void raise(const desktop_window_info& w) {
    set_state(w, false);
  }
  void minimize(const desktop_window_info& w) {
    set_state(w, true);
  }

  void close(const desktop_window_info& w) {
    key_t key = w.top_key;
    actions_.push_back({w.session, [key](context& s) {
                          auto it = s.top_level_objects.find(key);
                          if (it == s.top_level_objects.end() || !it->second)
                            return;
                          // Same event the title-bar X sends; the owning
                          // client decides whether the window goes away.
                          key_t saved = s.current_top_level_key;
                          s.current_top_level_key = key;
                          enqueue_event(s, it->second->wish_id(), "closed"_key, dynamic{});
                          s.current_top_level_key = saved;
                          s.dirty.store(kDirtySettleFrames, std::memory_order_release);
                        }});
  }

  /// The active window: the most recently focused one, unless minimized.
  /// Not simply "focused now": clicking the taskbar takes focus itself,
  /// and its buttons must still see which window was active before.
  bool is_active(const desktop_window_info& w) const {
    return !w.minimized && !st_.mru.empty() && st_.mru.front() == std::make_pair(w.session, w.top_key) &&
        any_focused_recently_;
  }

  const desktop_window_info* find_window(const sync_context* session, key_t key) const {
    for (const auto& w : f_.windows)
      if (w.session == session && w.top_key == key)
        return &w;
    return nullptr;
  }

  // ── Parts ──

  void update_window_order();
  void draw_wallpaper();
  void draw_desktop_icons(const ImRect& work);
  void draw_host(const ImRect& work);
  void draw_taskbar(float height);
  void draw_start_menu(const ImRect& anchor);
  void draw_pinned(float button);
  void draw_window_buttons(float button, float width);
  void draw_tray(float height);
  float tray_width(float button) const;
  void draw_calendar(const ImRect& anchor);
  void draw_switcher();
  void app_context_items(const desktop_app_info& app);

  imgui_renderer& r_;
  shell_frame& f_;
  desktop_shell_state& st_;
  std::vector<shell_action>& actions_;
  const std::function<void()>& shutdown_server_;
  float icon_px_{24.0f}; ///< Taskbar icon size, set from the bar height.
  /// Whether some entry, or the taskbar itself, holds focus (clicking the
  /// empty desktop clears it, so nothing shows as active).
  bool any_focused_recently_{false};
};

void shell_painter::update_window_order() {
  auto alive = [&](const std::pair<const sync_context*, key_t>& e) {
    return find_window(e.first, e.second) != nullptr;
  };
  auto prune = [&](auto& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return !alive(e); }), v.end());
  };
  prune(st_.taskbar_order);
  prune(st_.mru);
  for (const auto& w : f_.windows) {
    auto entry = std::make_pair(w.session, w.top_key);
    if (std::find(st_.taskbar_order.begin(), st_.taskbar_order.end(), entry) == st_.taskbar_order.end())
      st_.taskbar_order.push_back(entry);
    auto it = std::find(st_.mru.begin(), st_.mru.end(), entry);
    if (w.focused && !w.minimized) {
      if (it != st_.mru.end())
        st_.mru.erase(it);
      st_.mru.insert(st_.mru.begin(), entry);
    } else if (it == st_.mru.end()) {
      st_.mru.push_back(entry);
    }
  }
}

void shell_painter::draw() {
  update_window_order();
  {
    const ImGuiWindow* nav = GImGui->NavWindow;
    const char* nav_name = nav && nav->RootWindow ? nav->RootWindow->Name : "";
    bool chrome_focused = std::strncmp(nav_name, "##wish_taskbar", 14) == 0 ||
        std::strncmp(nav_name, "##wish_switcher", 15) == 0 || (nav && (nav->Flags & ImGuiWindowFlags_Popup));
    any_focused_recently_ = chrome_focused ||
        std::any_of(f_.windows.begin(), f_.windows.end(), [](const auto& w) { return w.focused && !w.minimized; });
  }

  ImGuiViewport* vp = ImGui::GetMainViewport();

  // Disable ImGui's own Ctrl+Tab window cycling: the shell's switcher
  // replaces it (and also restores minimized windows).
  ImGuiContext& g = *GImGui;
  if (!st_.nav_windowing_saved) {
    st_.saved_nav_next = g.ConfigNavWindowingKeyNext;
    st_.saved_nav_prev = g.ConfigNavWindowingKeyPrev;
    st_.nav_windowing_saved = true;
  }
  g.ConfigNavWindowingKeyNext = 0;
  g.ConfigNavWindowingKeyPrev = 0;

  draw_wallpaper();

  const float taskbar_h = std::floor(std::max(40.0f, ImGui::GetFrameHeight() * 2.0f));
  draw_taskbar(taskbar_h);

  // The side bar above reserved its strip of the work area immediately;
  // everything else lives in what's left.
  ImRect work = static_cast<ImGuiViewportP*>(vp)->GetBuildWorkRect();
  draw_desktop_icons(work);
  draw_host(work);
  r_.set_desktop_cascade_area(work.Min, work.Max);

  draw_switcher();
}

void shell_painter::draw_wallpaper() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImDrawList* dl = ImGui::GetBackgroundDrawList(const_cast<ImGuiViewport*>(vp));
  const ImVec2 p0 = vp->Pos;
  const ImVec2 p1(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);

  if (!f_.wallpaper.empty()) {
    if (ImTextureID tex = texture(f_.wallpaper)) {
      // Cover fit: fill the screen, cropping the image's overflowing axis
      // (stretch when the backend can't report the image size).
      ImVec2 uv0(0, 0), uv1(1, 1);
      ImVec2 ts = r_.texture_size(resolved(f_.wallpaper));
      if (ts.x > 0 && ts.y > 0 && vp->Size.x > 0 && vp->Size.y > 0) {
        float img_aspect = ts.x / ts.y;
        float scr_aspect = vp->Size.x / vp->Size.y;
        if (img_aspect > scr_aspect) {
          float visible = scr_aspect / img_aspect;
          uv0.x = (1.0f - visible) * 0.5f;
          uv1.x = 1.0f - uv0.x;
        } else {
          float visible = img_aspect / scr_aspect;
          uv0.y = (1.0f - visible) * 0.5f;
          uv1.y = 1.0f - uv0.y;
        }
      }
      dl->AddImage(ImTextureRef(tex), p0, p1, uv0, uv1);
      return;
    }
  }

  // Built-in wallpaper: a diagonal gradient with two soft glows, in the
  // theme's mood.
  ImU32 tl, tr, br, bl;
  if (f_.light) {
    tl = IM_COL32(142, 197, 252, 255);
    tr = IM_COL32(176, 190, 250, 255);
    br = IM_COL32(224, 195, 252, 255);
    bl = IM_COL32(190, 220, 250, 255);
  } else {
    tl = IM_COL32(16, 32, 64, 255);
    tr = IM_COL32(40, 36, 92, 255);
    br = IM_COL32(86, 44, 110, 255);
    bl = IM_COL32(18, 60, 96, 255);
  }
  dl->AddRectFilledMultiColor(p0, p1, tl, tr, br, bl);
  const float r = std::max(vp->Size.x, vp->Size.y);
  ImU32 glow = f_.light ? IM_COL32(255, 255, 255, 28) : IM_COL32(120, 160, 255, 22);
  dl->AddCircleFilled(ImVec2(p0.x + vp->Size.x * 0.78f, p0.y + vp->Size.y * 0.30f), r * 0.32f, glow, 96);
  dl->AddCircleFilled(ImVec2(p0.x + vp->Size.x * 0.15f, p0.y + vp->Size.y * 0.85f), r * 0.22f, glow, 96);
}

void shell_painter::draw_desktop_icons(const ImRect& work) {
  // Its own window, kept behind the host window: the host's passthrough
  // dockspace punches a hit-test hole over the empty central area, so
  // clicks there fall through to this window.
  ImGui::SetNextWindowPos(work.Min);
  ImGui::SetNextWindowSize(work.GetSize());
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoFocusOnAppearing;
  ImGui::Begin("##wish_desktop_icons", nullptr, flags);
  ImGui::PopStyleVar(2);
  ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());

  if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
    st_.selected_icon.clear();

  if (f_.show_icons) {
    const float cell_w = 92.0f;
    const float cell_h = 88.0f;
    const float icon = 40.0f;
    ImVec2 origin = ImGui::GetCursorScreenPos();
    int rows = std::max(1, int((work.GetHeight() - 24.0f) / cell_h));
    int index = 0;
    for (const auto& app : f_.apps) {
      if (!app.on_desktop)
        continue;
      ImVec2 cell_min(origin.x + float(index / rows) * cell_w, origin.y + float(index % rows) * cell_h);
      ++index;
      ImGui::SetCursorScreenPos(cell_min);
      ImGui::PushID(app.app.c_str());
      ImGui::InvisibleButton("##icon", ImVec2(cell_w - 4.0f, cell_h - 4.0f));
      bool hovered = ImGui::IsItemHovered();
      bool selected = st_.selected_icon == app.app;
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        st_.selected_icon = app.app;
      if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        launch(app);
      if (hovered && !app.description.empty())
        ImGui::SetItemTooltip("%s", app.description.c_str());
      if (ImGui::BeginPopupContextItem("##icon_ctx")) {
        if (ImGui::MenuItem("Open"))
          launch(app);
        app_context_items(app);
        ImGui::EndPopup();
      }

      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 cell_max(cell_min.x + cell_w - 4.0f, cell_min.y + cell_h - 4.0f);
      if (selected || hovered)
        dl->AddRectFilled(cell_min, cell_max, IM_COL32(255, 255, 255, selected ? 60 : 32), 6.0f);
      if (ImTextureID tex = app.icon.empty() ? texture(kDefaultAppIcon) : texture(app.icon)) {
        ImVec2 i0(cell_min.x + (cell_w - 4.0f - icon) * 0.5f, cell_min.y + 8.0f);
        dl->AddImage(
            ImTextureRef(tex),
            ImVec2(i0.x + 1, i0.y + 1),
            ImVec2(i0.x + icon + 1, i0.y + icon + 1),
            ImVec2(0, 0),
            ImVec2(1, 1),
            IM_COL32(0, 0, 0, 110));
        dl->AddImage(ImTextureRef(tex), i0, ImVec2(i0.x + icon, i0.y + icon));
      }
      // Label centered under the icon, shadowed for contrast on any wallpaper.
      std::string label = app.label;
      const float max_w = cell_w - 10.0f;
      while (label.size() > 3 && ImGui::CalcTextSize(label.c_str()).x > max_w)
        label = label.substr(0, label.size() - 4) + "...";
      ImVec2 ts = ImGui::CalcTextSize(label.c_str());
      ImVec2 tp(cell_min.x + (cell_w - 4.0f - ts.x) * 0.5f, cell_min.y + 8.0f + icon + 6.0f);
      dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label.c_str());
      dl->AddText(tp, IM_COL32(255, 255, 255, 255), label.c_str());
      ImGui::PopID();
    }
  }
  ImGui::End();
}

void shell_painter::draw_host(const ImRect& work) {
  ImGui::SetNextWindowPos(work.Min);
  ImGui::SetNextWindowSize(work.GetSize());
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  constexpr ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
  // Same window name and dockspace id as the plain host chrome, so docked
  // layouts carry over between the two modes.
  ImGui::Begin("##wish_host_chrome", nullptr, host_flags);
  ImGui::PopStyleVar(3);
  ImGuiID dock_id = ImGui::GetID("HostDockSpace");
  ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
  r_.set_ambient_dockspace_id(dock_id);
  ImGui::End();
}

void shell_painter::app_context_items(const desktop_app_info& app) {
  if (ImGui::MenuItem(app.pinned ? "Unpin from taskbar" : "Pin to taskbar"))
    set_pinned(app, !app.pinned);
  if (ImGui::MenuItem(app.on_desktop ? "Remove from desktop" : "Add to desktop"))
    set_on_desktop(app, !app.on_desktop);
}

void shell_painter::draw_taskbar(float height) {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
  bg.w = 0.88f;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavFocus;
  if (ImGui::BeginViewportSideBar("##wish_taskbar", vp, ImGuiDir_Down, height, flags)) {
    // Always drawn over floating windows dragged across it -- but not over
    // an open popup (Start menu, power menu, calendar, context menus).
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
      ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y), ImGui::GetColorU32(ImGuiCol_Border));

    const float button = height - 8.0f;
    icon_px_ = std::floor(button * 0.55f);
    ImGui::SetCursorPosY(4.0f);

    // Start button.
    ImVec2 start_min = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    if (bar_button("##wish_start", texture(kStartIcon), button, icon_px_ * 1.15f, accent(), "Start"))
      ImGui::OpenPopup("##wish_start_menu");
    ImGui::SetItemTooltip("Start");
    ImRect start_rect(start_min, ImGui::GetItemRectMax());
    if (ImGui::IsPopupOpen("##wish_start_menu") == false)
      st_.focus_search = true;
    draw_start_menu(start_rect);

    ImGui::SameLine(0.0f, 8.0f);
    draw_pinned(button);

    const float tray_w = tray_width(button);
    float avail = ImGui::GetContentRegionAvail().x - tray_w - 8.0f;
    draw_window_buttons(button, avail);
    ImGui::PopStyleColor();

    draw_tray(height);
  } else {
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}

void shell_painter::draw_start_menu(const ImRect& anchor) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 size(std::min(440.0f, vp->Size.x - 16.0f), std::min(560.0f, anchor.Min.y - vp->Pos.y - 12.0f));
  ImGui::SetNextWindowPos(ImVec2(anchor.Min.x, anchor.Min.y - 8.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowSize(size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
  ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
  bg.w = 0.98f; // some themes' popups are see-through; the menu is a panel
  ImGui::PushStyleColor(ImGuiCol_PopupBg, bg);
  bool open = ImGui::BeginPopup("##wish_start_menu");
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  if (!open)
    return;

  // Search box, focused when the menu opens: typing filters right away.
  if (ImTextureID tex = texture(kSearchIcon)) {
    float h = ImGui::GetFrameHeight();
    ImGui::ImageWithBg(ImTextureRef(tex), ImVec2(h, h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), text_color());
    ImGui::SameLine();
  }
  if (st_.focus_search) {
    ImGui::SetKeyboardFocusHere();
    st_.focus_search = false;
  }
  ImGui::SetNextItemWidth(-FLT_MIN);
  bool enter = ImGui::InputTextWithHint(
      "##wish_start_search",
      "Type to search apps",
      st_.search,
      sizeof(st_.search),
      ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::Spacing();

  const float footer_h = ImGui::GetFrameHeightWithSpacing() + 12.0f;
  ImGui::BeginChild("##wish_start_list", ImVec2(0.0f, -footer_h), ImGuiChildFlags_None);
  const std::string query = st_.search;
  const desktop_app_info* first_match = nullptr;
  std::string current_collection;
  bool any = false;
  // Without a search, apps are grouped by collection, in their given order;
  // with one, best matches (name prefix, then name, then description) come
  // first, in one flat list, and Enter opens the top one.
  std::vector<const desktop_app_info*> sorted;
  for (const auto& app : f_.apps)
    if (detail::desktop_app_matches(app, query))
      sorted.push_back(&app);
  if (query.empty()) {
    std::stable_sort(sorted.begin(), sorted.end(), [](const desktop_app_info* a, const desktop_app_info* b) {
      return a->collection < b->collection;
    });
  } else {
    std::stable_sort(sorted.begin(), sorted.end(), [&](const desktop_app_info* a, const desktop_app_info* b) {
      return detail::desktop_app_match_rank(*a, query) < detail::desktop_app_match_rank(*b, query);
    });
  }
  const float icon = ImGui::GetTextLineHeight() * 1.6f;
  for (const desktop_app_info* app : sorted) {
    if (!first_match)
      first_match = app;
    any = true;
    if (query.empty() && app->collection != current_collection) {
      current_collection = app->collection;
      if (!current_collection.empty())
        ImGui::SeparatorText(current_collection.c_str());
    }
    ImGui::PushID(app->app.c_str());
    ImVec2 row_min = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(0.0f, icon + 4.0f));
    if (!app->description.empty())
      ImGui::SetItemTooltip("%s", app->description.c_str());
    if (ImGui::BeginPopupContextItem("##app_ctx")) {
      if (ImGui::MenuItem("Open"))
        launch(*app);
      app_context_items(*app);
      ImGui::EndPopup();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImTextureID tex = app->icon.empty() ? texture(kDefaultAppIcon) : texture(app->icon))
      dl->AddImage(
          ImTextureRef(tex),
          ImVec2(row_min.x + 4.0f, row_min.y + 2.0f),
          ImVec2(row_min.x + 4.0f + icon, row_min.y + 2.0f + icon),
          ImVec2(0, 0),
          ImVec2(1, 1),
          ImGui::GetColorU32(ImGuiCol_CheckMark));
    float text_y = row_min.y + (icon + 4.0f - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(row_min.x + icon + 14.0f, text_y), ImGui::GetColorU32(ImGuiCol_Text), app->label.c_str());
    if (app->pinned) {
      if (ImTextureID pin = texture(kPinIcon)) {
        float px = row_min.x + ImGui::GetContentRegionAvail().x - ImGui::GetTextLineHeight() - 4.0f;
        float ph = ImGui::GetTextLineHeight();
        dl->AddImage(
            ImTextureRef(pin),
            ImVec2(px, text_y),
            ImVec2(px + ph, text_y + ph),
            ImVec2(0, 0),
            ImVec2(1, 1),
            ImGui::GetColorU32(ImGuiCol_TextDisabled));
      }
    }
    ImGui::PopID();
    if (clicked) {
      launch(*app);
      st_.search[0] = '\0';
      ImGui::CloseCurrentPopup();
    }
  }
  if (!any)
    ImGui::TextDisabled("%s", query.empty() ? "No apps installed" : "No apps match your search");
  ImGui::EndChild();

  if (enter && first_match) {
    launch(*first_match);
    st_.search[0] = '\0';
    ImGui::CloseCurrentPopup();
  }

  // Footer: user on the left, power menu on the right.
  ImGui::Separator();
  ImGui::Spacing();
  float h = ImGui::GetFrameHeight();
  if (ImTextureID tex = texture(kUserIcon)) {
    ImGui::ImageWithBg(ImTextureRef(tex), ImVec2(h, h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), text_color());
    ImGui::SameLine();
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(f_.user.empty() ? "wish" : f_.user.c_str());
  ImGui::SameLine(ImGui::GetContentRegionMax().x - h);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  bool power = false;
  if (ImTextureID tex = texture(kPowerIcon))
    power = icon_button("##wish_power", tex, text_color(), ImVec2(h, h));
  else
    power = ImGui::Button("Power");
  ImGui::PopStyleColor();
  ImGui::SetItemTooltip("Power");
  ImRect power_rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  if (power)
    ImGui::OpenPopup("##wish_power_menu");
  // Opens upward, like the Start menu itself.
  ImGui::SetNextWindowPos(ImVec2(power_rect.Max.x, power_rect.Min.y - 4.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  if (ImGui::BeginPopup("##wish_power_menu")) {
    if (ImGui::MenuItem("Quit desktop")) {
      emit("quit");
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Shut down server") && shutdown_server_)
      shutdown_server_();
    ImGui::EndPopup();
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void shell_painter::draw_pinned(float button) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  bool any = false;
  for (size_t i = 0; i < f_.apps.size(); ++i) {
    const auto& app = f_.apps[i];
    if (!app.pinned)
      continue;
    any = true;
    ImGui::PushID(app.app.c_str());
    ImTextureID tex = app_icon(int(i));
    bool clicked = bar_button("##pinned", tex, button, icon_px_, ImGui::GetColorU32(ImGuiCol_Text), app.label.c_str());
    ImGui::SetItemTooltip("%s", app.label.c_str());
    ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    bool running = !app.sessions.empty();
    if (running) {
      // Running indicator: a short accent line under the icon.
      float cx = (rect.Min.x + rect.Max.x) * 0.5f;
      dl->AddLine(ImVec2(cx - 6.0f, rect.Max.y + 1.0f), ImVec2(cx + 6.0f, rect.Max.y + 1.0f), accent(), 3.0f);
    }
    if (ImGui::BeginPopupContextItem("##pinned_ctx")) {
      if (ImGui::MenuItem(running ? "New window" : "Open"))
        launch(app);
      app_context_items(app);
      ImGui::EndPopup();
    }
    if (clicked) {
      // Like the Windows taskbar: a running app comes to the front (or its
      // windows all minimize when it already is); otherwise it starts.
      std::vector<const desktop_window_info*> mine;
      for (const auto& w : f_.windows)
        if (w.app_index == int(i))
          mine.push_back(&w);
      if (mine.empty()) {
        launch(app);
      } else {
        bool front = std::any_of(mine.begin(), mine.end(), [&](auto* w) { return is_active(*w); });
        for (auto* w : mine) {
          if (front)
            minimize(*w);
          else
            raise(*w);
        }
      }
    }
    ImGui::PopID();
    ImGui::SameLine();
  }
  if (any) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(
        ImVec2(p.x + 2.0f, p.y + 6.0f),
        ImVec2(p.x + 2.0f, p.y + button - 6.0f),
        ImGui::GetColorU32(ImGuiCol_Separator),
        1.0f);
    ImGui::Dummy(ImVec2(6.0f, button));
    ImGui::SameLine();
  }
}

void shell_painter::draw_window_buttons(float button, float width) {
  std::vector<const desktop_window_info*> list;
  for (const auto& e : st_.taskbar_order)
    if (const auto* w = find_window(e.first, e.second))
      list.push_back(w);
  if (list.empty() || width <= button)
    return;

  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  float each = std::min(200.0f, (width - spacing * float(list.size())) / float(list.size()));
  each = std::max(each, button);
  const float icon = icon_px_ * 0.85f;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  for (const desktop_window_info* w : list) {
    if (ImGui::GetContentRegionAvail().x < each)
      break; // out of room; the switcher still reaches the rest
    ImGui::PushID(w->session);
    ImGui::PushID(int(w->top_key.id));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##win", ImVec2(each, button));
    bool hovered = ImGui::IsItemHovered();
    ImVec2 p1 = ImGui::GetItemRectMax();
    bool active = is_active(*w);
    if (active || hovered)
      dl->AddRectFilled(
          p0, p1, ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered, 0.6f), 6.0f);
    // Underline: wide accent for the active window, short for others.
    float cx = (p0.x + p1.x) * 0.5f;
    float half = active ? (p1.x - p0.x) * 0.3f : 6.0f;
    dl->AddLine(
        ImVec2(cx - half, p1.y - 1.5f),
        ImVec2(cx + half, p1.y - 1.5f),
        w->minimized ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : accent(),
        3.0f);
    float pad = 8.0f;
    if (ImTextureID tex = app_icon(w->app_index))
      dl->AddImage(
          ImTextureRef(tex),
          ImVec2(p0.x + pad, p0.y + (button - icon) * 0.5f),
          ImVec2(p0.x + pad + icon, p0.y + (button + icon) * 0.5f),
          ImVec2(0, 0),
          ImVec2(1, 1),
          ImGui::GetColorU32(ImGuiCol_Text));
    float text_x = p0.x + pad + icon + 6.0f;
    float text_w = p1.x - pad - text_x;
    if (text_w > 12.0f) {
      ImVec2 tp(text_x, p0.y + (button - ImGui::GetTextLineHeight()) * 0.5f);
      ImGui::RenderTextEllipsis(dl, tp, ImVec2(p1.x - pad, p1.y), p1.x - pad, w->title.c_str(), nullptr, nullptr);
    }
    ImGui::SetItemTooltip("%s", w->title.c_str());
    if (clicked) {
      if (active)
        minimize(*w);
      else
        raise(*w);
    }
    if (ImGui::BeginPopupContextItem("##win_ctx")) {
      if (w->minimized ? ImGui::MenuItem("Restore") : ImGui::MenuItem("Minimize")) {
        if (w->minimized)
          raise(*w);
        else
          minimize(*w);
      }
      if (w->app_index >= 0) {
        const auto& app = f_.apps[size_t(w->app_index)];
        if (ImGui::MenuItem("New window"))
          launch(app);
        if (ImGui::MenuItem(app.pinned ? "Unpin from taskbar" : "Pin to taskbar"))
          set_pinned(app, !app.pinned);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Close window", nullptr, false, w->closable))
        close(*w);
      ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SameLine();
  }
}

float shell_painter::tray_width(float button) const {
  std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  float clock = std::max(
                    ImGui::CalcTextSize(detail::desktop_format_time(f_.time_format, now).c_str()).x,
                    ImGui::CalcTextSize(detail::desktop_format_time(f_.date_format, now).c_str()).x) +
      20.0f;
  float clients = ImGui::GetTextLineHeight() + 6.0f + ImGui::CalcTextSize("000").x;
  return clients + button + clock + 10.0f + 3.0f * ImGui::GetStyle().ItemSpacing.x + 8.0f;
}

void shell_painter::draw_tray(float height) {
  const float button = height - 8.0f;
  const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
  ImGui::SetCursorPos(ImVec2(right - tray_width(button) + 8.0f, 4.0f));
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Connected clients.
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float ih = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(ih + 6.0f + ImGui::CalcTextSize("000").x, button));
    ImGui::SetItemTooltip("%d connected app%s", int(f_.clients), f_.clients == 1 ? "" : "s");
    float y = p.y + (button - ih) * 0.5f;
    if (ImTextureID tex = texture(kClientsIcon))
      dl->AddImage(
          ImTextureRef(tex),
          ImVec2(p.x, y),
          ImVec2(p.x + ih, y + ih),
          ImVec2(0, 0),
          ImVec2(1, 1),
          ImGui::GetColorU32(ImGuiCol_Text));
    std::string n = std::to_string(f_.clients);
    dl->AddText(ImVec2(p.x + ih + 6.0f, y), ImGui::GetColorU32(ImGuiCol_Text), n.c_str());
    ImGui::SameLine();
  }

  // Theme toggle.
  {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    bool clicked = bar_button(
        "##wish_theme",
        texture(f_.light ? kDarkIcon : kLightIcon),
        button,
        icon_px_ * 0.8f,
        ImGui::GetColorU32(ImGuiCol_Text),
        f_.light ? "Dark" : "Light");
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%s", f_.light ? "Switch to dark theme" : "Switch to light theme");
    if (clicked) {
      dynamic p;
      p["preset"_key] = std::string{f_.light ? "dark" : "light"};
      emit("theme", std::move(p));
    }
    ImGui::SameLine();
  }

  // Clock: time over date; click for the calendar.
  {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::string time_s = detail::desktop_format_time(f_.time_format, now);
    std::string date_s = detail::desktop_format_time(f_.date_format, now);
    float w = std::max(ImGui::CalcTextSize(time_s.c_str()).x, ImGui::CalcTextSize(date_s.c_str()).x) + 20.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##wish_clock", ImVec2(w, button))) {
      std::tm local{};
#if defined(_WIN32)
      localtime_s(&local, &now);
#else
      localtime_r(&now, &local);
#endif
      st_.calendar_year = local.tm_year + 1900;
      st_.calendar_month = local.tm_mon;
      ImGui::OpenPopup("##wish_calendar");
    }
    if (ImGui::IsItemHovered())
      dl->AddRectFilled(p0, ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.6f), 6.0f);
    float lh = ImGui::GetTextLineHeight();
    float y = p0.y + (button - 2.0f * lh) * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(ImVec2(p0.x + (w - ImGui::CalcTextSize(time_s.c_str()).x) * 0.5f, y), col, time_s.c_str());
    dl->AddText(ImVec2(p0.x + (w - ImGui::CalcTextSize(date_s.c_str()).x) * 0.5f, y + lh), col, date_s.c_str());
    draw_calendar(ImRect(p0, ImGui::GetItemRectMax()));
    ImGui::SameLine();
  }

  // Show desktop: a thin strip at the far right edge.
  {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##wish_show_desktop", ImVec2(10.0f, button))) {
      bool any_visible = std::any_of(f_.windows.begin(), f_.windows.end(), [](const auto& w) { return !w.minimized; });
      if (any_visible) {
        st_.shown_desktop.clear();
        for (const auto& w : f_.windows) {
          if (!w.minimized) {
            st_.shown_desktop.emplace_back(w.session, w.top_key);
            minimize(w);
          }
        }
      } else {
        for (const auto& e : st_.shown_desktop)
          if (const auto* w = find_window(e.first, e.second))
            raise(*w);
        st_.shown_desktop.clear();
      }
    }
    ImGui::SetItemTooltip("Show desktop");
    ImVec2 p1 = ImGui::GetItemRectMax();
    dl->AddLine(ImVec2(p0.x, p0.y + 4.0f), ImVec2(p0.x, p1.y - 4.0f), ImGui::GetColorU32(ImGuiCol_Separator));
    if (ImGui::IsItemHovered())
      dl->AddRectFilled(ImVec2(p0.x + 2.0f, p0.y), p1, ImGui::GetColorU32(ImGuiCol_ButtonHovered, 0.6f), 3.0f);
  }
}

void shell_painter::draw_calendar(const ImRect& anchor) {
  ImGui::SetNextWindowPos(ImVec2(anchor.Max.x, anchor.Min.y - 8.0f), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
  if (!ImGui::BeginPopup("##wish_calendar")) {
    ImGui::PopStyleVar(2);
    return;
  }
  ImGui::PopStyleVar(2);

  std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm today{};
#if defined(_WIN32)
  localtime_s(&today, &now);
#else
  localtime_r(&now, &today);
#endif
  ImGui::TextUnformatted(detail::desktop_format_time("%A, %d %B %Y", now).c_str());
  ImGui::Separator();

  // Month header with previous/next arrows.
  std::tm first{};
  first.tm_year = st_.calendar_year - 1900;
  first.tm_mon = st_.calendar_month;
  first.tm_mday = 1;
  char month_name[64]{};
  std::strftime(month_name, sizeof(month_name), "%B %Y", &first);
  const float cell = ImGui::GetFrameHeight() + 6.0f;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(month_name);
  ImGui::SameLine(cell * 7.0f - 2.0f * ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::ArrowButton("##prev_month", ImGuiDir_Left)) {
    if (--st_.calendar_month < 0) {
      st_.calendar_month = 11;
      --st_.calendar_year;
    }
  }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##next_month", ImGuiDir_Right)) {
    if (++st_.calendar_month > 11) {
      st_.calendar_month = 0;
      ++st_.calendar_year;
    }
  }

  static constexpr std::array<const char*, 7> kDays{"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetCursorScreenPos();
  for (int d = 0; d < 7; ++d) {
    ImVec2 ts = ImGui::CalcTextSize(kDays[size_t(d)]);
    dl->AddText(
        ImVec2(origin.x + cell * float(d) + (cell - ts.x) * 0.5f, origin.y),
        ImGui::GetColorU32(ImGuiCol_TextDisabled),
        kDays[size_t(d)]);
  }
  origin.y += ImGui::GetTextLineHeightWithSpacing() + 2.0f;
  int offset = detail::desktop_first_weekday(st_.calendar_year, st_.calendar_month);
  int days = detail::desktop_days_in_month(st_.calendar_year, st_.calendar_month);
  bool this_month = today.tm_year + 1900 == st_.calendar_year && today.tm_mon == st_.calendar_month;
  int rows = (offset + days + 6) / 7;
  for (int day = 1; day <= days; ++day) {
    int slot = offset + day - 1;
    ImVec2 c0(origin.x + cell * float(slot % 7), origin.y + cell * float(slot / 7));
    ImVec2 center(c0.x + cell * 0.5f, c0.y + cell * 0.5f);
    std::string label = std::to_string(day);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    bool is_today = this_month && day == today.tm_mday;
    if (is_today)
      dl->AddCircleFilled(center, cell * 0.42f, accent());
    dl->AddText(
        ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
        is_today ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text),
        label.c_str());
  }
  ImGui::Dummy(ImVec2(cell * 7.0f, ImGui::GetTextLineHeightWithSpacing() + 2.0f + cell * float(rows)));
  ImGui::EndPopup();
}

void shell_painter::draw_switcher() {
  ImGuiIO& io = ImGui::GetIO();
  // Ctrl+Tab cycles; Ctrl+` too, since a browser keeps Ctrl+Tab for itself
  // (web renderer).
  bool next =
      io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Tab, true) || ImGui::IsKeyPressed(ImGuiKey_GraveAccent, true));
  std::vector<const desktop_window_info*> list;
  for (const auto& e : st_.mru)
    if (const auto* w = find_window(e.first, e.second))
      list.push_back(w);

  if (next && !list.empty()) {
    int step = io.KeyShift ? -1 : 1;
    if (!st_.switcher_open) {
      st_.switcher_open = true;
      // Start on the previous window, unless the front one isn't active.
      bool front_active = is_active(*list.front());
      st_.switcher_index = front_active ? (list.size() > 1 ? (step > 0 ? 1 : int(list.size()) - 1) : 0) : 0;
    } else {
      st_.switcher_index = (st_.switcher_index + step + int(list.size())) % int(list.size());
    }
  }
  if (!st_.switcher_open)
    return;
  if (list.empty() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    st_.switcher_open = false;
    return;
  }
  st_.switcher_index = std::clamp(st_.switcher_index, 0, int(list.size()) - 1);
  if (!io.KeyCtrl) {
    raise(*list[size_t(st_.switcher_index)]);
    st_.switcher_open = false;
    return;
  }

  const float tile = 132.0f;
  const float icon = 48.0f;
  const int per_row = std::max(1, std::min(int(list.size()), 6));
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoInputs;
  ImGui::Begin("##wish_switcher", nullptr, flags);
  ImGui::PopStyleVar(2);
  ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (size_t i = 0; i < list.size(); ++i) {
    if (i % size_t(per_row) != 0)
      ImGui::SameLine();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(tile, tile * 0.8f));
    ImVec2 p1(p0.x + tile, p0.y + tile * 0.8f);
    if (int(i) == st_.switcher_index)
      dl->AddRect(p0, p1, accent(), 8.0f, 0, 3.0f);
    if (ImTextureID tex = app_icon(list[i]->app_index))
      dl->AddImage(
          ImTextureRef(tex),
          ImVec2(p0.x + (tile - icon) * 0.5f, p0.y + 14.0f),
          ImVec2(p0.x + (tile + icon) * 0.5f, p0.y + 14.0f + icon),
          ImVec2(0, 0),
          ImVec2(1, 1),
          ImGui::GetColorU32(ImGuiCol_Text));
    ImVec2 tp(p0.x + 8.0f, p0.y + 14.0f + icon + 10.0f);
    ImGui::RenderTextEllipsis(dl, tp, ImVec2(p1.x - 8.0f, p1.y), p1.x - 8.0f, list[i]->title.c_str(), nullptr, nullptr);
  }
  ImGui::End();
}

} // namespace

// ── Entry point ──────────────────────────────────────────────────────────────

bool render_desktop_shell(
    imgui_renderer& r,
    const std::vector<sync_context_ptr>& sessions,
    desktop_shell_state& state,
    const std::function<void()>& shutdown_server) {
  auto frame = snapshot(r, sessions);
  if (!frame) {
    // Back to a plain host: hand ImGui its own Ctrl+Tab cycling again.
    if (state.nav_windowing_saved) {
      GImGui->ConfigNavWindowingKeyNext = state.saved_nav_next;
      GImGui->ConfigNavWindowingKeyPrev = state.saved_nav_prev;
      state.nav_windowing_saved = false;
    }
    return false;
  }

  std::vector<shell_action> actions;
  {
    // Draw with the shell session's own theme, so the theme toggle restyles
    // the chrome together with the client windows sharing that session.
    ImGuiStyle saved = ImGui::GetStyle();
    if (frame->style)
      ImGui::GetStyle() = *frame->style;
    try {
      shell_painter(r, *frame, state, actions, shutdown_server).draw();
    } catch (...) {
      ImGui::GetStyle() = saved;
      throw;
    }
    ImGui::GetStyle() = saved;
  }

  for (const auto& sync_ctx : sessions) {
    bool has_action =
        std::any_of(actions.begin(), actions.end(), [&](const shell_action& a) { return a.session == sync_ctx.get(); });
    if (!has_action)
      continue;
    auto sess = context_wlock{*sync_ctx};
    for (auto& a : actions)
      if (a.session == sync_ctx.get())
        a.apply(*sess);
  }
  return true;
}

} // namespace bdg::wish
