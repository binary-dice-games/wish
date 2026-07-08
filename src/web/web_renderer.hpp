// MIT License © 2025 Binary Dice Games
/// @file web_renderer.hpp
/// @brief Browser-backed concrete renderer: runs Dear ImGui headlessly and
///        streams draw data to a browser over HTTP + WebSocket.
#pragma once

#ifdef WISH_WEB_ENABLED

#include <imgui/imgui_renderer.hpp>
#include <web/civetweb_server.hpp>
#include <web/draw_protocol.hpp>

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bdg::wish {

/**
 * @brief Headless renderer built on Dear ImGui, streamed to a browser.
 *
 * Lifecycle (all methods called from the render thread, never the
 * constructor/destructor thread):
 *
 *   setup()         — creates the ImGui/ImPlot/ImPlot3D contexts, no GPU
 *                      or platform backend (headless)
 *   N × begin_frame / render_node / end_frame
 *   teardown()      — destroys the contexts
 *
 * Unlike `sdl3_renderer`, `should_quit()` defaults to `false` and is never
 * set automatically (e.g. on zero connected clients) — the server runs
 * until `request_quit()` is called explicitly. See `src/web/DESIGN.md`.
 */
class web_renderer : public imgui_renderer {
 public:
  /**
   * @brief Construct with HTTP/WebSocket bind parameters.
   *
   * Does **not** start listening or create the ImGui context — that
   * happens in `setup()`, which is called from the render thread.
   *
   * @param bind_addr  Address to bind the HTTP/WebSocket server to.
   * @param port       TCP port for the HTTP/WebSocket server.
   * @param font_size  Base font size in pixels.
   */
  explicit web_renderer(std::string bind_addr = "127.0.0.1", int port = 8080, int font_size = 16);

  ~web_renderer() override;

  // ── renderer lifecycle ────────────────────────────────────────────────────

  /// @brief Create the ImGui/ImPlot/ImPlot3D contexts (headless, no
  ///        backend), extract the embedded browser client assets to a
  ///        process-global temp directory, and start the HTTP server.
  void setup() override;

  /// @brief Stop the HTTP server, remove the extracted asset directory, and
  ///        destroy the ImGui/ImPlot/ImPlot3D contexts.
  void teardown() override;

  // ── per-frame ─────────────────────────────────────────────────────────────

  /// @brief `true` if any input event, resize, connect, or disconnect has
  ///        happened since the last call (and clears that state) -- lets
  ///        `wish::server::render_loop` skip drawing frames no browser is
  ///        looking at or interacting with.
  bool poll_events() override;

  /**
   * @brief Drains the inbound input-event queue and any pending resize into
   *        `ImGui::GetIO()`, then forwards to `imgui_renderer::begin_frame()`
   *        (ImGui::NewFrame).
   */
  void begin_frame() override;

  /**
   * @brief Calls `ImGui::Render()` itself (like `sdl3_renderer`, since the
   *        base `imgui_renderer::end_frame()` only calls `ImGui::EndFrame()`),
   *        then walks `ImDrawData::Textures` to broadcast texture
   *        (re)uploads, sends any newly-connected client's initial texture
   *        sync, and broadcasts the encoded FRAME.
   */
  void end_frame() override;

  /// @brief Returns `true` only once `request_quit()` has been called.
  bool should_quit() const override;

  /// @brief Programmatically request shutdown.
  void request_quit();

  /// @brief Directory the embedded browser client assets were extracted
  ///        to; empty until `setup()` has run.
  const std::filesystem::path& web_assets_dir() const {
    return web_assets_dir_;
  }

  /// @brief The actual bound HTTP/WebSocket port (resolves ephemeral port
  ///        `0`); `0` if `setup()` has not run.
  int actual_port() const {
    return server_ ? server_->actual_port() : 0;
  }

 private:
  std::string bind_addr_;
  int port_;
  int font_size_;
  std::atomic<bool> quit_{false};

  std::unique_ptr<civetweb_server> server_;
  std::filesystem::path web_assets_dir_;

  // Connections that have completed the WS handshake since the last
  // end_frame() but have not yet received their initial texture sync.
  // Populated on a civetweb worker thread (on_connect), drained on the
  // render thread (end_frame()).
  bison::synchronized<std::vector<ws_connection_id>> pending_sync_;

  // Inbound input events decoded on a civetweb worker thread (on_message),
  // drained into ImGuiIO on the render thread at the top of begin_frame().
  bison::synchronized<std::deque<web_input_event>> input_queue_;

  // Only the most recent resize matters; no need to queue every one.
  bison::synchronized<std::optional<web_resize_event>> pending_resize_;

  // Set by on_connect/on_disconnect/on_message (civetweb worker threads),
  // consumed by poll_events() (render thread). A single bool needs no
  // synchronized<T> wrapper -- mirrors sdl3_renderer::quit_.
  std::atomic<bool> activity_{false};

  // wish-assigned texture ids, keyed by the (stable, per ImGui docs)
  // ImTextureData* pointer. Render-thread only -- never touched from a
  // civetweb worker thread -- so no synchronized<T> wrapper is needed.
  std::unordered_map<ImTextureData*, uint32_t> texture_ids_;
  uint32_t next_texture_id_ = 1;
};

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
