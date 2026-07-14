// MIT License © 2025 Binary Dice Games
/**
 * @file host_renderer.cpp
 * @brief Out-of-line implementation of host_renderer<Base>, explicitly
 *        instantiated below for every backend that uses it.
 */
#include "app/wish_cli/host_renderer.hpp"

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
void host_renderer<Base>::render_server_frame(const std::vector<sync_context_ptr>& sessions) {
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
      if (ImGui::MenuItem("Quit", "Alt+F4"))
        this->request_quit();
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

  ImGui::End();
}

#ifdef WISH_SDL3_ENABLED
template class host_renderer<sdl3_renderer>;
#endif
#ifdef WISH_WEB_ENABLED
template class host_renderer<web_renderer>;
#endif

} // namespace bdg::wish
