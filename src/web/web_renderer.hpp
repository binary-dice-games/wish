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
#include <unordered_set>
#include <utility>
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

  // ── texture loading ───────────────────────────────────────────────────────

  /**
   * @brief Decode an image file with `stb_image` and register it as an ImGui
   *        user texture so the next `end_frame()` uploads it to every
   *        connected browser via the normal `ImDrawData::Textures` path.
   *
   * Results are cached by `src` (not the base class's `texture_cache_`,
   * which can only hold a settled id -- see `loaded_by_src_`), so repeated
   * calls for the same path are free after the first. Returns a null handle
   * (zero) -- cached, so decoding is not retried -- if the file cannot be
   * found or decoded.
   *
   * Unlike `sdl3_renderer`, the returned `ImTextureID` is never valid on the
   * frame it is first requested: the id is only assigned once `end_frame()`
   * calls `ImGui::Render()`, which happens strictly after `render_node()`
   * (and therefore after this call returns). Callers (e.g. the `Image`
   * element) already treat a null id as "nothing to draw yet" for that
   * frame, matching `get_or_load_font()`'s first-call contract; the texture
   * is available from the following frame onward.
   *
   * @param src              Filename relative to the session resource directory.
   * @param resource_dir     Session-scoped resource folder.
   * @param embedded_crc32s  Optional map of resource_dir-relative path ->
   *                          precomputed CRC-32 for embedded assets (see
   *                          `context::embedded_crc32s`). When `src` (relative
   *                          to `resource_dir`) has an entry here, it is
   *                          reused as the texture's content-version number
   *                          instead of recomputing a CRC-32 over the file's
   *                          bytes; otherwise (e.g. session-uploaded files,
   *                          which never carry a precomputed checksum) the
   *                          CRC-32 is computed on the fly. Used to decide
   *                          whether the browser resource cache may be
   *                          offered this texture — see `texture_meta_`.
   */
  ImTextureID get_or_load_texture(const std::string& src, const std::filesystem::path& resource_dir,
      const std::unordered_map<std::string, uint32_t>* embedded_crc32s = nullptr) override;

  /// Per-texture identity/versioning metadata for the browser resource
  /// cache, populated by get_or_load_texture(). Textures with no such
  /// metadata (the font atlas, and any other user texture not registered
  /// through get_or_load_texture()) have no stable on-disk identity and are
  /// never offered to the browser cache.
  struct texture_meta {
    std::string src;         ///< Path relative to `resource_dir`.
    uint32_t crc32 = 0;      ///< Content version (see get_or_load_texture()).
    bool cacheable = false;  ///< False for anything under a `private/` prefix.
  };

  /// @brief Test-support accessor: the cache metadata recorded for a texture
  ///        previously loaded via `get_or_load_texture()` for this exact
  ///        `src`, or `std::nullopt` if none was recorded (e.g. the file
  ///        failed to decode). Not used by production code -- exists so
  ///        tests can observe the CRC32/cacheable decision without needing
  ///        the full TEX_CHECK wire protocol handshake.
  std::optional<texture_meta> texture_meta_for_test(const std::string& src) const;

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

  // Every connection currently known to be open (inserted in on_connect,
  // erased in on_disconnect) -- unlike pending_sync_, this is never
  // drained. Lets end_frame()'s WantCreate handling offer a cacheable
  // texture's very first upload as a TEX_CHECK to whichever clients are
  // already connected at that moment, not just ones that join later via
  // pending_sync_ -- see "Persistent Browser Resource Cache" in
  // src/web/DESIGN.md.
  bison::synchronized<std::unordered_set<ws_connection_id>> connected_ids_;

  // Inbound input events decoded on a civetweb worker thread (on_message),
  // drained into ImGuiIO on the render thread at the top of begin_frame().
  bison::synchronized<std::deque<web_input_event>> input_queue_;

  // Only the most recent resize matters; no need to queue every one.
  bison::synchronized<std::optional<web_resize_event>> pending_resize_;

  // CACHE_RESPONSE messages decoded on a civetweb worker thread (on_message),
  // drained on the render thread at the top of end_frame() (mirrors
  // input_queue_).
  bison::synchronized<std::deque<std::pair<ws_connection_id, web_cache_response>>> cache_response_queue_;

  // Per-connection texture ids with an outstanding TEX_CHECK the browser
  // hasn't answered yet. Populated in end_frame()'s pending_sync_ drain
  // (render thread only); consumed (erased) when the matching
  // CACHE_RESPONSE is drained from cache_response_queue_ -- both happen on
  // the render thread, but the map itself is written to from
  // pending_sync_'s render-thread code only, so this synchronized<T> exists
  // purely so a future consumer on another thread doesn't have to reason
  // about it -- see cache_response_queue_ for the actual cross-thread
  // handoff.
  bison::synchronized<std::unordered_map<ws_connection_id, std::unordered_set<uint32_t>>> awaiting_cache_response_;

  // Set by on_connect/on_disconnect/on_message (civetweb worker threads),
  // consumed by poll_events() (render thread). A single bool needs no
  // synchronized<T> wrapper -- mirrors sdl3_renderer::quit_.
  std::atomic<bool> activity_{false};

  // wish-assigned texture ids, keyed by the (stable, per ImGui docs)
  // ImTextureData* pointer. Render-thread only -- never touched from a
  // civetweb worker thread -- so no synchronized<T> wrapper is needed.
  std::unordered_map<ImTextureData*, uint32_t> texture_ids_;
  uint32_t next_texture_id_ = 1;

  // Reverse of texture_ids_: needed to rebuild a full TEX_CREATE payload
  // once a per-connection cache miss is reported (see end_frame()'s
  // cache_response_queue_ drain), without re-walking ImDrawData::Textures to
  // find the ImTextureData* for a given wire id. Render-thread only, kept in
  // sync wherever texture_ids_ is.
  std::unordered_map<uint32_t, ImTextureData*> textures_by_id_;

  // Owns every ImTextureData created by get_or_load_texture(); registered
  // with ImGui via ImGui::RegisterUserTexture() so it flows through the same
  // ImDrawData::Textures upload path as the font atlas. Freed (and
  // unregistered) in teardown().
  std::vector<std::unique_ptr<ImTextureData>> loaded_textures_;

  // Cache of get_or_load_texture()'s decode results, keyed by `src`. Not the
  // base class's `texture_cache_` (which only stores a settled ImTextureID)
  // because a freshly-created ImTextureData's id isn't known until end_frame()
  // walks ImDrawData::Textures -- storing the ImTextureData* instead lets
  // get_or_load_texture() read tex->TexID fresh on every call. Null entries
  // mark a `src` that failed to decode, so a bad path isn't retried every frame.
  std::unordered_map<std::string, ImTextureData*> loaded_by_src_;

  // Per-texture identity/versioning metadata for the browser resource
  // cache, populated by get_or_load_texture() alongside `loaded_by_src_`.
  std::unordered_map<ImTextureData*, texture_meta> texture_meta_;
};

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
