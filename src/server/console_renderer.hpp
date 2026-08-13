// MIT License © 2025 Binary Dice Games
/// @file console_renderer.hpp
/// @brief Lightweight text-dump renderer, for headless/CI use.
#pragma once

#include <server/renderer.hpp>

#include <string>

namespace bdg::wish {

/**
 * @brief Renderer that prints the widget tree to stdout instead of drawing
 *        anything.
 *
 * No display, SDL3, or web dependency -- meant for the `wish_server_dll` C
 * ABI's `renderer_kind="console"` option (see `wish_server_c.h`), so tests
 * and CI can exercise a real `bdg::wish::server` session (real per-widget
 * proxies, real events) without a display. Not a substitute for the real
 * `sdl3`/`web` renderers as a UI a person actually looks at.
 */
class console_renderer : public renderer {
 public:
  void begin_frame() override {}
  void render_node(const ui_element& node, const context& s) override;
  void end_frame() override {}

  /// Only draw (print) a frame when a session is actually dirty (an RMI
  /// dispatch changed something), not on every render_loop tick -- avoids
  /// reprinting an unchanged tree ~60 times a second. See renderer.hpp's
  /// doc comment on this flag.
  bool render_on_demand() const override {
    return true;
  }

 private:
  int depth_ = 0;
};

} // namespace bdg::wish
