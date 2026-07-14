// MIT License © 2025 Binary Dice Games
/**
 * @file host_renderer.hpp
 * @brief Shared host-window chrome (dockspace + menu bar) for imgui-based
 *        renderer backends.
 */
#pragma once

#include <context/context.hpp>
#include <ui/ui_element.hpp>

#include <vector>

namespace bdg::wish {

/**
 * @brief Extends a given imgui-based renderer backend (sdl3_renderer or
 *        web_renderer) with a fullscreen host window that provides:
 *          - A DockSpace so client windows can be docked anywhere in the
 *            host view.
 *          - A menu bar with host-level actions (including Quit),
 *            extensible by sessions that register a MenuBarExtension
 *            top-level object.
 *
 * Templatized on the backend so the same host-window UI is shared by every
 * renderer that draws through ImGui, rather than duplicating it per backend.
 * Used by both `wish server` (one host shared by every connected session)
 * and `wish standalone` (one host for the single embedded session) --
 * standalone otherwise has no DockSpaceViewport for its app's windows to
 * dock into.
 *
 * `render_server_frame()` is defined out-of-line in host_renderer.cpp, which
 * explicitly instantiates `host_renderer<sdl3_renderer>` and
 * `host_renderer<web_renderer>` -- the only two specializations ever used --
 * so this header stays free of the ImGui-heavy render body as the chrome
 * grows, and callers only need the declaration to use the type.
 */
template <typename Base>
class host_renderer : public Base {
 public:
  using Base::Base;

  void render_server_frame(const std::vector<sync_context_ptr>& sessions) override;
};

} // namespace bdg::wish
