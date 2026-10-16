// MIT License © 2025 Binary Dice Games
/**
 * @file host_renderer.cpp
 * @brief Out-of-line implementation of host_renderer<Base>, explicitly
 *        instantiated below for every backend that uses it.
 */
#include "app/wish_cli/host_renderer.hpp"

#include <imgui/imgui_notifications.hpp>
#include <sdl/sdl3_renderer.hpp>
#include <ui/ui_elements/ui_elements.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>

namespace bdg::wish {

namespace {

// Returns the session's MenuBarExtension top-level object, if it registered
// one, else nullptr.  Sessions (e.g. the desktop bridge) use this class to
// splice extra menu-bar content into the host's own chrome instead of
// creating a competing menu bar/dockspace.
const ui_element* find_menu_bar_extension(const context& s) {
  for (const auto& [key, win] : s.top_level_objects) {
    if (win && win->as<bison::key_t>(bison::dynamic::CLASS) == bison::key_t{"MenuBarExtension"})
      return win.get();
  }
  return nullptr;
}

} // namespace

template <typename Base>
void host_renderer<Base>::enable_server_overlay(server_overlay_info info) {
  if (!info.shutdown)
    info.shutdown = [this] { this->request_quit(); };
  if (!info.request_screenshot)
    info.request_screenshot = [this](std::function<void(std::vector<uint8_t>)> done) {
      return this->request_screenshot(std::move(done));
    };
  info.perf = &perf_;
  overlay_ = std::make_unique<server_overlay>(std::move(info), this->notifications());
  // Closing the window asks for confirmation (render_server_frame()).
  if constexpr (requires(Base& b) { b.set_confirm_close(true); })
    this->set_confirm_close(true);
  if (overlay_->has_hotkey()) {
    notification hint;
    hint.title = "wish server is running";
    hint.body = "Press to open the server overlay: clients, settings, logs and more.";
    hint.keys = overlay_->hotkey();
    hint.duration = std::chrono::seconds{10};
    hint.tag = "wish.server.overlay.hint";
    this->notifications()->post(std::move(hint));
  }
}

template <typename Base>
void host_renderer<Base>::request_server_shutdown() {
  if (overlay_)
    overlay_->request_shutdown();
  else
    this->request_quit();
}

template <typename Base>
void host_renderer<Base>::render_server_overlay(const std::vector<sync_context_ptr>& sessions) {
  if (overlay_)
    overlay_->render(sessions);
  // A screenshot frame shows the clients' windows only.
  // Toasts stay clear of the overlay's action bar (its Shut down button sits
  // in the same corner).
  notifications_visible_ = !(overlay_ && overlay_->capturing()) &&
      render_notifications(*this->notifications(), notification_corner::bottom_right,
                           overlay_ ? overlay_->bottom_bar_height() : 0.0f);
}

template <typename Base>
void host_renderer<Base>::begin_frame() {
  frame_start_ = std::chrono::steady_clock::now();
  Base::begin_frame();
}

template <typename Base>
void host_renderer<Base>::end_frame() {
  Base::end_frame();
  perf_.record_frame(std::chrono::steady_clock::now() - frame_start_);
}

template <typename Base>
void host_renderer<Base>::tick(const std::vector<sync_context_ptr>& sessions) {
  Base::tick(sessions);
  const auto now = std::chrono::steady_clock::now();
  if (perf_.due(now))
    perf_.close_sample(now, sessions.size(), process_cpu_seconds(), process_memory_bytes());
}

template <typename Base>
bool host_renderer<Base>::wants_continuous_redraw() const {
  return Base::wants_continuous_redraw() || notifications_visible_ ||
      (overlay_ && (overlay_->is_open() || overlay_->capturing() || overlay_->confirming_shutdown()));
}

template <typename Base>
bool host_renderer<Base>::consume_render_request() {
  const bool requested = Base::consume_render_request();
  const bool notified = this->notifications()->consume_changed();
  return requested || notified;
}

template <typename Base>
void host_renderer<Base>::render_server_frame(const std::vector<sync_context_ptr>& sessions) {
  // The overlay's hotkey first, so it toggles whatever the sessions do
  // with the keyboard this frame.
  if (overlay_)
    overlay_->handle_keys();
  if constexpr (requires(Base& b) { b.consume_close_request(); }) {
    if (this->consume_close_request())
      request_server_shutdown();
  }

  // A DesktopShell session turns the host into a desktop; its chrome
  // replaces the menu bar host window below entirely.
  if (render_desktop_shell(*this, sessions, desktop_state_, [this] { request_server_shutdown(); }))
    return;

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  // Host window fills the whole viewport, so its background reads as the
  // app's canvas rather than a widget surface -- use the theme's dedicated
  // "empty docking node" color (ImGuiCol_DockingEmptyBg) instead of
  // ImGuiCol_WindowBg, which the dark preset sets close to black.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg]);

  constexpr ImGuiWindowFlags host_flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_MenuBar;

  ImGui::Begin("##wish_host_chrome", nullptr, host_flags);
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("Server")) {
      if (overlay_ && ImGui::MenuItem("Overlay", overlay_->hotkey().c_str()))
        overlay_->set_open(true);
      if (ImGui::MenuItem("Quit", "Alt+F4"))
        request_server_shutdown();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("(no options yet)", nullptr, false, false);
      ImGui::EndMenu();
    }

    // Splice in any session-registered MenuBarExtension content (e.g. the
    // desktop bridge's File menu and clock), one session locked at a time
    // -- mirrors the lock discipline server::render_loop uses for its own
    // per-session rendering, just for this smaller subtree and up front.
    for (const auto& sync_ctx : sessions) {
      auto sess = context_wlock{*sync_ctx};
      if (const ui_element* ext = find_menu_bar_extension(*sess)) {
        detail::current_context = &*sess;
        render_children(*this, *ext, *sess);
        detail::current_context = nullptr;
      }
    }

    ImGui::EndMenuBar();
  }

  ImGuiID dock_id = ImGui::GetID("HostDockSpace");
  ImGui::DockSpace(dock_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
  // Every connected session's un-positioned, dockable top-level Windows dock
  // into this shell by default (see render_window). Set each frame, before
  // the per-session render pass in server::render_loop().
  this->set_ambient_dockspace_id(dock_id);

  ImGui::End();
}

#ifdef WISH_SDL3_ENABLED
template class host_renderer<sdl3_renderer>;
#endif
#ifdef WISH_WEB_ENABLED
template class host_renderer<web_renderer>;
#endif

} // namespace bdg::wish
