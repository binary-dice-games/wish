// MIT License © 2025 Binary Dice Games
/// @file web_renderer.hpp
/// @brief Browser-backed concrete renderer: runs Dear ImGui headlessly and
///        streams draw data to a browser over HTTP + WebSocket.
#pragma once

#ifdef WISH_WEB_ENABLED

#include <imgui/imgui_renderer.hpp>
#include <web/civetweb_server.hpp>
#include <web/draw_protocol.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_query.hpp>
#endif

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
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
   * @param extra_render_fns  Extra/override `render_*` dispatch entries, see
   *                          `imgui_renderer::imgui_renderer()`.
   * @param render_on_demand  See `renderer::render_on_demand()`. Only
   *                          meaningful (and only ever settable to
   *                          anything other than the initially-idle
   *                          default) when built with
   *                          `WISH_AUTOMATION_ENABLED` -- nothing can ever
   *                          call `request_render()` otherwise, since the
   *                          only trigger is the REQUEST_RENDER wire
   *                          message, decoded only in that build
   *                          configuration.
   */
  explicit web_renderer(std::string bind_addr = "127.0.0.1", int port = 8080, int font_size = 16,
      render_fn_map extra_render_fns = {}, bool render_on_demand = false);

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

  bool render_on_demand() const override {
    return render_on_demand_;
  }
  void request_render() override {
    render_requested_.store(true, std::memory_order_relaxed);
  }
  bool consume_render_request() override {
    return render_requested_.exchange(false, std::memory_order_relaxed);
  }

#ifdef WISH_AUTOMATION_ENABLED
  // ── automation ───────────────────────────────────────────────────────────

  /**
   * @brief Capture this node's screen rect and interaction flags into
   *        `hit_test_map_` after drawing it via the base `imgui_renderer`.
   *
   * Wrap-and-capture, exactly the shape `tests/test_imgui_renderer.cpp`'s
   * `counting_imgui_renderer` already proves safe: calls
   * `imgui_renderer::render_node()` first (unchanged dispatch/recursion),
   * then reads `ImGui::GetItemRectMin/Max()` etc. for the node just drawn.
   * A node with no `__wish_id` field (never assigned one, e.g. a manually
   * built tree in a unit test) is skipped -- `hit_test_map_` only tracks
   * automation-addressable widgets.
   */
  void render_node(const ui_element& node, const context& s) override;

  /// @brief See `imgui_renderer::capture_hit_test_for_last_item()` -- used by
  ///        `render_table()` for `TableRow`, whose row-spanning `Selectable`
  ///        is drawn inline rather than via `render_node()`.
  void capture_hit_test_for_last_item(const ui_element& node) override;

  /**
   * @brief Answer any QUERY_TREE requests queued since the last call, and
   *        push a LOG_EVENT for any log entries logged since the last call.
   *
   * Called by `wish::server::render_loop` / `wish::standalone::render_loop`
   * right after this session's `render_session()` calls complete, while the
   * session's context write-lock is still held -- see
   * `src/automation/DESIGN.md`. At that point `hit_test_map_` holds exactly
   * this frame's rects for @p s (cleared fresh in `begin_frame()`), so a
   * QUERY_TREE reply always reflects the most recently completed frame.
   *
   * Log broadcasting is unconditional (no browser request needed): every
   * call compares `s.logger_service->recent_logs()` against
   * `last_broadcast_log_seq_` and broadcasts anything new to every
   * connected browser, in the order `log()` was called -- so an automation
   * script sees log events land in sequence with its own actions (e.g.
   * "click a button, then observe the log entry it caused") rather than
   * having to ask for logs and reconstruct timing itself. A no-op when
   * @p s has no logger service attached yet.
   *
   * @param s  The session whose `ui_objects` / `logger_service` this call
   *           should act on.
   */
  void service_automation_queries(const context& s) override;

  /**
   * @brief Answer any QUERY_TREE requests queued since the last call, with
   *        an empty widget list, since no session is connected to
   *        introspect.
   *
   * Called instead of `service_automation_queries(const context&)` for any
   * frame where zero RMI sessions are connected -- see that overload's
   * base-class doc comment (`renderer::service_automation_queries()`) for
   * why this exists. Does not push `LOG_EVENT`s: there is no session, so
   * no `logger_service` to read from.
   */
  void service_automation_queries() override;
#endif

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

  // ── font loading ─────────────────────────────────────────────────────────

  /**
   * @brief Load a TTF font at @p size pixels via ImGui's dynamic font atlas
   *        and return the cached `ImFont*`.
   *
   * `setup()` opts into `ImGuiBackendFlags_RendererHasTextures`, so unlike
   * `sdl3_renderer::rebuild_font_atlas()` there is no static-atlas rebuild
   * to trigger here: `io.Fonts->AddFontFromFileTTF()` may be called at any
   * time (including mid-frame), and the resulting font's glyph texture(s)
   * stream to the browser through the exact same `ImDrawData::Textures`
   * walk `end_frame()` already performs for every other texture -- see
   * `ImGui::ShowFontSelector()`'s "RendererHasTextures" branch in
   * imgui.cpp for the upstream documentation of this.
   *
   * Still returns `nullptr` (default font) on the first call for a given
   * (path, size) -- matching `get_or_load_texture()`'s first-call contract
   * above -- because the browser hasn't received the new texture's pixels
   * yet on the frame it's requested; from the following call onward the
   * cached font is returned immediately.
   *
   * @param path  Fully-resolved absolute path to a TTF font file.
   * @param size  Font size in pixels.
   */
  ImFont* get_or_load_font(const std::string& path, float size) override;

  // ── offscreen render target ──────────────────────────────────────────────

  /**
   * @brief Assign (or reuse) an offscreen render-target id and redirect
   *        `flush_draw_list()` to tag its `FRAME` broadcasts with it instead
   *        of the canvas.
   *
   * There is no GPU here to redirect draws to server-side -- unlike
   * `sdl3_renderer`, the actual offscreen framebuffer/texture is created and
   * owned by the *browser* (`client.js`'s `Renderer.renderTargets`), keyed
   * by the id this returns. Mirrors `sdl3_renderer::begin_render_target()`'s
   * single-slot design: one cached id/size, recreated (tearing down the old
   * id via an ordinary `TEX_DESTROY`) only when the requested size changes.
   * See "Offscreen Render Targets" in `src/web/DESIGN.md`.
   */
  ImTextureID begin_render_target(int w, int h) override;

  /// @brief Restore the target id active before the matching
  ///        `begin_render_target()` call.
  void end_render_target() override;

  /**
   * @brief Immediately broadcast @p draw_list as a `FRAME` tagged with
   *        whatever render-target id is currently active (`0` if none --
   *        the canvas), instead of waiting for `end_frame()`'s own
   *        broadcast.
   *
   * Mirrors `sdl3_renderer::flush_draw_list()`: wraps @p draw_list in a
   * throwaway `ImDrawData` (no texture uploads to service -- everything
   * `draw_list` references was already uploaded earlier this frame).
   */
  void flush_draw_list(ImDrawList& draw_list, int w, int h) override;

 private:
  // ImGuiPlatformIO::Platform_GetClipboardTextFn / Platform_SetClipboardTextFn
  // callbacks (see "Clipboard bridging" in src/web/DESIGN.md) -- NOT the
  // older ImGuiIO::GetClipboardTextFn/SetClipboardTextFn (which ImGui's own
  // GetClipboardText()/SetClipboardText() no longer calls directly; a
  // legacy-to-PlatformIO remap exists but is not reliably active by the
  // time NewFrame() first runs). Platform_* callbacks take an
  // ImGuiContext*, not a void* user_data, so there is no per-callback way
  // to recover `this` -- setup()/teardown() instead point a static
  // instance pointer at whichever web_renderer currently owns the ImGui
  // context, mirroring the "one active renderer per process" assumption
  // already made elsewhere (e.g. sdl3_renderer).
  static const char* get_clipboard_text(ImGuiContext* ctx);
  static void set_clipboard_text(ImGuiContext* ctx, const char* text);

  /**
   * @brief `true` if a browser mouse-move to (@p x, @p y) is far/old enough
   *        from the last significant one to warrant drawing a frame.
   *
   * Mirrors `sdl3_renderer::mouse_motion_significant()` (4 px / 100 ms): a
   * bare hover over the canvas should not force a full re-render +
   * `encode_frame` + broadcast every frame the way it did when every inbound
   * message unconditionally set `activity_`. The move is still queued into
   * `input_queue_` regardless, so `begin_frame()` feeds ImGuiIO the exact
   * cursor path on the frames that do render. Called from civetweb worker
   * threads (`on_message`), so the baseline it updates is guarded by
   * `mouse_motion_filter_`.
   */
  bool mouse_move_significant(float x, float y);

  std::string bind_addr_;
  int port_;
  int font_size_;
  std::atomic<bool> quit_{false};
  const bool render_on_demand_;
  // Set by request_render() (via a REQUEST_RENDER wire message decoded in
  // on_message(), a civetweb worker thread), consumed by
  // consume_render_request() on the render thread -- mirrors activity_'s
  // identical worker-thread-write/render-thread-read shape.
  std::atomic<bool> render_requested_{false};

  std::unique_ptr<civetweb_server> server_;
  std::filesystem::path web_assets_dir_;

  // Backing storage for ImGuiIO::IniFilename -- ImGui keeps the pointer we
  // assign it, so the path string must outlive the ImGui context. Snapshot
  // as an absolute path at setup() time so a later chdir() elsewhere in the
  // process can't move where imgui.ini is read from/written to out from
  // under a long-running server.
  std::string ini_path_;

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

  // Most recent OS clipboard text the browser has told us about (see
  // "Clipboard bridging" in src/web/DESIGN.md), consumed synchronously by
  // the ImGuiIO::GetClipboardTextFn callback -- there's no per-frame queue
  // to drain here (unlike input_queue_) because that callback fires
  // on-demand, mid-frame, whenever ImGui processes a paste keystroke.
  bison::synchronized<std::string> pending_clipboard_text_;

  // Backing storage for GetClipboardTextFn's returned `const char*`, which
  // per ImGui's contract must stay valid until the next clipboard call --
  // render-thread only (that callback only ever runs there).
  std::string clipboard_text_scratch_;

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
  //
  // A mouse-move message only sets this when mouse_move_significant() says so
  // (mirroring sdl3_renderer's poll_events() motion debounce); every other
  // inbound message kind still sets it unconditionally.
  std::atomic<bool> activity_{false};

  // Baseline for mouse_move_significant(), written only from civetweb worker
  // threads (on_message). Mirrors sdl3_renderer::last_motion_x_/y_/time_/
  // has_motion_baseline_, wrapped because multiple connections' on_message
  // callbacks can run on different worker threads.
  struct motion_filter {
    float last_x = 0.0f;
    float last_y = 0.0f;
    std::chrono::steady_clock::time_point last_time{};
    bool has_baseline = false;
  };
  bison::synchronized<motion_filter> mouse_motion_filter_;

  // Reused every begin_frame() as the drain target for input_queue_, instead
  // of move-constructing a fresh deque each frame.
  std::deque<web_input_event> input_scratch_;

  // Reused every end_frame() to build the broadcast FRAME message in place
  // (see draw_protocol::encode_frame()'s out-param overload) -- render-thread
  // only, keeps its grown capacity across frames.
  std::vector<std::byte> frame_scratch_;

  // Left/right modifier key state, tracked from individual LeftShift/
  // RightShift/... key events so begin_frame() can additionally emit the
  // merged ImGuiMod_Shift/Ctrl/Alt/Super event. ImGui derives io.KeyShift/
  // io.KeyMods (and thus ConfigDockingWithShift) from these merged events,
  // not from LeftShift/RightShift directly -- and critically, so does
  // every Ctrl/Shift/Alt/Super-modified shortcut check anywhere in ImGui
  // (IsKeyDown(ImGuiMod_Ctrl) and everything built on it, including
  // TextEditor's own Ctrl+A/C/V/X). Those merged states live in their own
  // pseudo-key slots (ImGuiKey_ReservedForModCtrl, etc.), entirely separate
  // from ImGuiKey_LeftCtrl/RightCtrl -- sending only the literal Left/Right
  // events, as this renderer used to, leaves that slot permanently "up", so
  // no Ctrl-modified shortcut is ever detected. See
  // ImGui_ImplSDL3_UpdateKeyModifiers() in imgui_impl_sdl3.cpp for the
  // reference backend doing the same thing from native OS modifier state.
  // Render-thread only, like input_queue_'s consumer side.
  struct modifier_state {
    bool left = false;
    bool right = false;
    bool any() const { return left || right; }
  };
  modifier_state mod_ctrl_;
  modifier_state mod_shift_;
  modifier_state mod_alt_;
  modifier_state mod_super_;

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

  // Font cache, keyed by (resolved path, pixel size) -- mirrors
  // sdl3_renderer::FontKey/font_cache_. The `ImFont*` is stored as soon as
  // it's loaded (first call for a key), but get_or_load_font() still
  // returns nullptr that same call; see its doc comment above.
  struct FontKey {
    std::string path;
    float size{0.0f};
    bool operator<(const FontKey& o) const {
      if (path != o.path)
        return path < o.path;
      return size < o.size;
    }
  };
  std::map<FontKey, ImFont*> font_cache_;

  // ── Offscreen render target ────────────────────────────────────────────────
  //
  // Mirrors sdl3_renderer::render_target_/render_target_w_/render_target_h_/
  // saved_render_target_, id-based instead of GPU-texture-based since there
  // is no ambient server-side render target to save/restore -- see
  // "Offscreen Render Targets" in src/web/DESIGN.md. Render-thread only,
  // like the rest of this backend's per-frame state.

  /// Current offscreen render-target id, recreated by begin_render_target()
  /// only when the requested size changes (the old id torn down via an
  /// ordinary TEX_DESTROY broadcast first). 0 = none allocated yet.
  uint32_t render_target_id_ = 0;
  int render_target_w_ = 0;
  int render_target_h_ = 0;

  /// What flush_draw_list() tags its FRAME broadcast with. 0 = canvas.
  uint32_t current_target_id_ = 0;

  /// current_target_id_'s value immediately before the last
  /// begin_render_target() call, restored by end_render_target().
  uint32_t saved_render_target_id_ = 0;

#ifdef WISH_AUTOMATION_ENABLED
  // Screen rect + interaction flags per widget, keyed by __wish_id, for the
  // frame currently being drawn. Render-thread only (written by render_node(),
  // read by service_automation_queries() -- both always called from the
  // render thread -- see src/automation/DESIGN.md). Cleared at the top of
  // begin_frame() so a query always answers against a complete frame, never
  // a partially-rendered one.
  automation::hit_test_map hit_test_map_;

  // QUERY_TREE requests decoded on a civetweb worker thread (on_message),
  // drained by service_automation_queries() on the render thread (mirrors
  // input_queue_). Each entry is the requesting connection paired with the
  // request's still-raw JSON payload; parsing happens on the render thread
  // in service_automation_queries() via automation::parse_query_tree_request().
  bison::synchronized<std::deque<std::pair<ws_connection_id, std::string>>> pending_tree_queries_;

  // Highest logger::log_entry::seq already broadcast as a LOG_EVENT.
  // Render-thread only (read and written solely inside
  // service_automation_queries()). Starts at 0, which is always less than
  // any real seq (logger::next_log_seq_ starts at 1), so the first call
  // broadcasts everything already buffered when automation started
  // watching. Session-wide, not per-session, matching hit_test_map_'s own
  // single-session assumption (see "Session model" in
  // src/automation/DESIGN.md) -- with more than one connected RMI session
  // this would under- or over-broadcast across them.
  uint64_t last_broadcast_log_seq_ = 0;
#endif
};

} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
