// MIT License © 2025 Binary Dice Games
/// @file renderer.hpp
/// @brief Abstract renderer interface and render-children utility for wish.
#pragma once

#include <context/context.hpp>
#include <ui/ui_element.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_backend.hpp>
#endif

#include <vector>

namespace bdg::wish {

/**
 * @brief Abstract rendering backend contract.
 *
 * Concrete backends (imgui, Qt, null) implement the three virtual methods.
 * `render_node` is the hot path: called once per visible element per frame.
 * After drawing the node itself, implementations should call
 * `render_children(r, node, s)` to recurse into children in render order.
 */
class renderer {
 public:
  virtual ~renderer() = default;

  /**
   * @brief Called once from the render thread before the first frame.
   *
   * Windowed backends (e.g. sdl3_renderer) use this to initialize platform
   * resources (SDL window, GPU context, ImGui backends) on the thread that
   * will own them for the duration of the session.  The default is a no-op.
   */
  virtual void setup() {}

  /**
   * @brief Called once from the render thread after the last frame exits.
   *
   * Must mirror every resource acquired in `setup()`.  Default is a no-op.
   */
  virtual void teardown() {}

  /**
   * @brief Returns true when the backend wants the server to stop.
   *
   * Windowed backends set this when the user closes the window.  The render
   * loop checks this after each frame and stops itself when it returns true.
   * Default returns false (server runs until `stop()` is called explicitly).
   */
  virtual bool should_quit() const {
    return false;
  }

  /**
   * @brief Poll platform/OS events for one render_loop iteration.
   *
   * Called every iteration of `wish::server::render_loop` /
   * `wish::standalone::render_loop`, whether or not a full frame ends up
   * being drawn, so OS event queues are always drained and window-close or
   * resize events are never missed.
   *
   * @return `true` if the events observed (input, window activity, a
   *         pending internal rebuild such as a font atlas reload, etc.)
   *         mean a full frame should be drawn this iteration. The default
   *         implementation — used by backends with no OS event loop, e.g.
   *         `null_renderer` or headless test renderers — always returns
   *         `true`, so those backends keep rendering every iteration.
   */
  virtual bool poll_events() {
    return true;
  }

  /**
   * @brief Returns true when something on screen animates over time and
   *        therefore needs continuous redraws even with no new input and no
   *        dirty session (e.g. a text-input widget's blinking caret).
   *
   * Checked once after each drawn frame; when true the render loop keeps
   * rendering every iteration (subject to the frame-rate cap) instead of
   * going idle. Default returns false.
   */
  virtual bool wants_continuous_redraw() const {
    return false;
  }

  /// @brief Called once before any nodes are rendered in a frame.
  virtual void begin_frame() = 0;

  /**
   * @brief Optional server-level UI rendered once per frame before client
   *        sessions.
   *
   * Override to inject a host window (e.g. a fullscreen dockspace with a menu
   * bar) that surrounds client-session windows.  The default is a no-op.
   * Called from `wish::server::render_loop` after `begin_frame` and before
   * any session is rendered.
   *
   * @param sessions Snapshot of currently-connected sessions.  Implementations
   *                 that extend the host chrome (e.g. a menu bar) may briefly
   *                 lock each session to look for top-level objects of a
   *                 well-known class (e.g. `MenuBarExtension`) and splice
   *                 their content into the host UI -- see
   *                 `host_renderer` in
   *                 `app/wish_cli/host_renderer.hpp`.
   */
  virtual void render_server_frame(const std::vector<sync_context_ptr>& sessions) {
    (void)sessions;
  }

  /**
   * @brief Render one session's complete element tree.
   *
   * The default implementation calls `render_node(root, s)` directly.
   * Backends that support per-session styling (e.g. imgui_renderer) override
   * this to apply a session-scoped visual style around the render call using
   * a RAII guard, so each session can have an independent theme without
   * polluting the global renderer state.
   *
   * @param root  Root element of the session's object tree.
   * @param s     Active session (style, events, resources).
   */
  virtual void render_session(const ui_element& root, const context& s) {
    render_node(root, s);
  }

  /**
   * @brief Draw a single UI element.
   *
   * Implementations dispatch on `node.as<bison::key_t>(bison::dynamic::CLASS)`
   * and call the appropriate backend draw primitive.  After drawing, call
   * `render_children(*this, node, s)` to recurse into the element's children.
   *
   * @param node  The element to draw.
   * @param s     Active session (used for event emission and resource lookup).
   */
  virtual void render_node(const ui_element& node, const context& s) = 0;

  /// @brief Called once after all nodes have been rendered in a frame.
  virtual void end_frame() = 0;

  /**
   * @brief Service the automation module for @p s: answer any pending
   *        tree/hit-test queries, and push any newly-logged entries.
   *
   * Called from `wish::server::render_loop` / `wish::standalone::render_loop`
   * right after @p s's `render_session()` calls for this frame complete,
   * while @p s's context write-lock is still held. The default is a no-op;
   * only `web_renderer` overrides it, gated by `WISH_AUTOMATION_ENABLED` --
   * see `src/automation/DESIGN.md`. Kept as a renderer-agnostic virtual hook
   * (rather than a `dynamic_cast`/`#ifdef WISH_WEB_ENABLED` special case in
   * the render loop) so `server.cpp`/`standalone.cpp` never need to know
   * which concrete renderer is active.
   *
   * @param s  Session this call should act on.
   */
  virtual void service_automation_queries(const context& s) {
    (void)s;
  }

  /**
   * @brief Answer pending tree/hit-test queries when no session is
   * connected.
   *
   * `pending_tree_queries_` (`web_renderer`'s queue) is populated purely by
   * the automation browser's own WebSocket connection and is independent of
   * any RMI session, but `wish::server::render_loop()` /
   * `wish::standalone::render_loop()` only call the `service_automation_queries(s)`
   * overload once per connected session -- if zero sessions are connected,
   * that per-session loop body never runs at all, so a `QUERY_TREE` sent
   * while the app-under-test hasn't connected yet (or has already
   * disconnected) would sit in the queue forever and the browser's
   * `getTree()` promise would never resolve. This overload is called once
   * per frame instead, only when the per-session path was skipped, so those
   * requests are still answered -- with an empty widget list, since there is
   * no session to introspect. default no-op; only `web_renderer` overrides
   * it.
   */
  virtual void service_automation_queries() {
  }

#ifdef WISH_AUTOMATION_ENABLED
  /**
   * @brief Returns this renderer as an `automation::automation_backend`, if
   *        it implements native (ABI-driven) automation.
   *
   * Checked once per new session, in `server::on_session_created`/
   * `standalone::on_session_created`: when non-null, the session gets an
   * `automation_service` ("__WishAutomation") that forwards RMI calls here.
   * The default returns `nullptr`; only backends that implement
   * `automation::automation_backend` (currently `sdl3_renderer`) override
   * this. See `src/automation/DESIGN.md`'s "Native (ABI-based) automation"
   * section.
   */
  virtual automation::automation_backend* as_automation_backend() {
    return nullptr;
  }
#endif
};

/**
 * @brief Iterate @p node's children in render order and call
 *        `r.render_node` for each.
 *
 * Uses `node.for_each_child_ordered` so children are visited in ascending
 * `order` field sequence (cache built at import time).  Concrete backends
 * call this from within `render_node` after drawing the node itself.
 *
 * @param r     The active renderer.
 * @param node  Parent element whose children to visit.
 * @param s     Active session forwarded to each `render_node` call.
 */
void render_children(renderer& r, const ui_element& node, const context& s);

/**
 * @brief No-op renderer for use in tests that do not require drawing.
 *
 * All virtual methods are empty stubs.  Does NOT recurse into children,
 * so `render_children` must be called explicitly when tests need recursion.
 */
class null_renderer : public renderer {
 public:
  void begin_frame() override {}
  void render_node(const ui_element&, const context&) override {}
  void end_frame() override {}
};

} // namespace bdg::wish
