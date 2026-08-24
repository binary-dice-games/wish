// MIT License © 2025 Binary Dice Games
/// @file server.cpp
/// @brief Implementation of wish::server.
#include <context/file_service.hpp>
#include <ui/forms/form.hpp>
#include <context/logger.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/style_service.hpp>
#include "src/rmi/shared/profiling.hpp"
#include <ui/ui_root.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_service.hpp>
#endif

#include "ui/ui_template.hpp"

#include <chrono>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace bdg::wish {

// Thread-local context pointer set by on_before_dispatch and cleared by
// on_after_dispatch.  Form and template-handler code reads session data
// through this pointer so they never re-acquire the already-held wlock (held
// for the whole dispatch by bison::rmi::server::client_worker).
thread_local context* detail::current_context = nullptr;

using namespace bison::rmi::transport;

namespace {

// Validates that `identity` is safe to use as a single path segment under
// persistent_sandbox_root_: non-empty, and containing none of '/', '\', or
// ".." -- the same class of check file_service::resolve_path applies to
// client-supplied file names, but simpler, since an identity is exactly one
// path segment rather than an arbitrary relative path.
bool is_safe_identity(const std::string& identity) {
  return !identity.empty() && identity.find('/') == std::string::npos && identity.find('\\') == std::string::npos &&
      identity.find("..") == std::string::npos;
}

} // namespace

// ── server ────────────────────────────────────────────────────────────────────

server::server(server_transport_iface& transport, std::unique_ptr<renderer> r)
    : bison::rmi::server(transport), renderer_(std::move(r)) {}

server::~server() {
  if (running_.load(std::memory_order_acquire)) {
    try {
      stop();
    } catch (...) {
    }
  }
}

void server::start(bison::rmi::auth_module_ptr auth_module, bison::dynamic listen_params) {
  register_all();
  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this]() { render_loop(); });
  listen(std::move(listen_params), std::move(auth_module));
}

void server::stop() {
  running_.store(false, std::memory_order_release);
  bison::rmi::server::stop();
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
}

bool server::should_quit() const {
  return !running_.load(std::memory_order_acquire);
}

std::unique_ptr<bison::rmi::context> server::on_create_context(bison::key_t session_id) {
  return std::make_unique<wish::context>(session_id);
}

void server::on_session_created(bison::rmi::context& ctx) {
  // The base class (client_worker) already holds this context's wlock for
  // the duration of this call, and has already registered it in
  // session_contexts() -- nothing left to do here but attach services.
  auto& s = static_cast<context&>(ctx);
  s.allow_absolute_paths = allow_absolute_paths_;
  s.allow_url_fetch = allow_url_fetch_;
  s.file_service = file_service::instantiate(s.resource_dir);
  s.style_service = style_service::instantiate();
  s.style_service->set_preset(default_theme_);
  // All sessions share the same global logger instance (set via set_logger()).
  s.logger_service = logger_;
#ifdef WISH_AUTOMATION_ENABLED
  if (renderer_) {
    if (auto* backend = renderer_->as_automation_backend())
      s.automation_service = automation_service::instantiate(backend, logger_);
  }
#endif
  {
    std::ostringstream oss;
    oss << "[rmi] connect     sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
    on_print(ctx.session_id, oss.str());
  }
  on_session_created(s);
}

void server::on_session_destroyed(bison::rmi::context& ctx) {
  // The base class holds this context's wlock for the duration of this call;
  // it erases the entry from session_contexts() after this returns.
  auto& s = static_cast<context&>(ctx);
  try {
    std::ostringstream oss;
    oss << "[rmi] disconnect  sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
    on_print(ctx.session_id, oss.str());
    on_session_destroyed(s);
  } catch (...) {
  }
}

void server::on_authenticated(bison::rmi::context& ctx, const std::string& identity) {
  // The base class holds this context's wlock for the duration of this
  // call, as it does for the whole OP_CONNECT dispatch that triggers it.
  if (persistent_sandbox_root_.empty() || identity.empty())
    return; // no persistence configured, or module extracted no identity
  if (!is_safe_identity(identity)) {
    on_print(ctx.session_id, "[rmi] rejected unsafe identity for persistent sandbox: " + identity);
    return;
  }

  auto& s = static_cast<context&>(ctx);
  s.resource_dir = persistent_sandbox_root_ / identity;
  s.resource_dir_persistent = true;
  s.populate_resource_dir();
  // Re-instantiate so the singleton __WishFileSystem object handed out by
  // find_singleton_service()/on_create_object() points at the persistent
  // directory instead of the temp one on_session_created() set up.
  s.file_service = file_service::instantiate(s.resource_dir);
}

bison::dynamic_ptr server::on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) {
  // on_create_object runs inside a dispatch, so ctx is the same object
  // detail::current_context already points at.
  auto& s = static_cast<context&>(ctx);

  if (auto svc = detail::find_singleton_service(s, klass))
    return svc;

  // form/ui_template hold a sync_context_ptr long-term (e.g. so destructors
  // can lock it outside of dispatch); look it up from the base class's own
  // session_contexts() map -- the single source of truth for this session.
  sync_context_ptr sync_ctx;
  {
    auto lp = session_contexts().rlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end())
      sync_ctx = it->second;
  }

  // For all other classes, bison creates the concrete type from the registered
  // prototype.  Inject session context into ui_template and form instances.
  auto obj = bison::rmi::server::on_create_object(ctx, ns, klass);
  detail::init_session_object(obj, ctx, sync_ctx);
  return obj;
}

void server::on_print(bison::key_t /*session_id*/, const std::string& line) {
  if (logger_)
    logger_->info(line);
}

void server::on_before_dispatch(bison::rmi::context& ctx) {
  detail::current_context = &static_cast<context&>(ctx);
}

void server::on_after_dispatch(bison::rmi::context& ctx) noexcept {
  // Any dispatched RMI call may have mutated session state (properties,
  // tree structure, style); flag the session so the render loop redraws it
  // instead of skipping the next idle-check.
  static_cast<context&>(ctx).dirty.store(kDirtySettleFrames, std::memory_order_release);
  detail::current_context = nullptr;
}

void server::render_loop() {
  if (renderer_)
    renderer_->setup();
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      BISON_TRACE_SCOPE("render_loop_tick");
      // poll_events() must run every iteration regardless of whether a
      // frame is drawn, so OS event queues are drained and window-close
      // requests are never delayed by an idle skip below.
      bool had_activity = renderer_->poll_events();

      // Snapshot the set of sync_context pointers under a brief
      // session_contexts() rlock; reused below both for the cheap dirty scan
      // and (if needed) for rendering/ticking. This lets different sessions
      // render without blocking each other and keeps session_contexts()
      // unblocked for session lifecycle operations.
      std::vector<sync_context_ptr> sessions_snapshot;
      {
        auto lp = session_contexts().rlock();
        sessions_snapshot.reserve(lp->size());
        for (const auto& [id, sp] : *lp)
          sessions_snapshot.push_back(sp);
      }

      // Simulation ticks at a steady ~60 Hz cadence regardless of whether a
      // frame actually gets drawn this iteration -- see renderer::tick()'s
      // doc comment. This is what lets render_on_demand() skip drawing
      // without also pausing whatever time-based state (e.g. genie's game
      // simulation) the app owns.
      static constexpr std::chrono::milliseconds kMinTickInterval{16}; // ~60 Hz
      auto now = std::chrono::steady_clock::now();
      if (now - last_tick_time_ >= kMinTickInterval) {
        last_tick_time_ = now;
        BISON_TRACE_SCOPE("tick");
        renderer_->tick(sessions_snapshot);
      }

      // render_on_demand() renderers don't treat routine poll_events()
      // activity (WS traffic, resize, ...) as a reason to draw -- only
      // genuine dirty (RMI dispatch, the initial post-connect settle
      // window) or an explicit request_render() call does. Everything else
      // is unaffected.
      if (had_activity && !renderer_->render_on_demand()) {
        pending_render_ = true;
        // Some ImGui-internal transitions triggered by raw input enqueue no
        // wish event at all (e.g. clicking a window's title-bar collapse
        // arrow: ButtonBehavior() sets ImGuiWindow::WantCollapseToggle this
        // frame, but Begin() only applies it -- actually flipping
        // window->Collapsed -- at the top of the *next* Begin() call). The
        // "!events.empty()" settle-frame bump further below only fires for
        // dispatched wish events, so a change like this can be computed
        // internally by ImGui but never actually rendered until unrelated
        // input (e.g. a later mouse move) happens to trigger poll_events()
        // again. Arm every session's settle-frame counter here too so a
        // couple of follow-up frames are always scheduled after raw input.
        for (const auto& sync_ctx : sessions_snapshot)
          context_wlock{*sync_ctx}->dirty.store(kDirtySettleFrames, std::memory_order_release);
      }
      if (renderer_->consume_render_request())
        pending_render_ = true;

      bool needs_render = pending_render_;
      if (!needs_render) {
        for (const auto& sync_ctx : sessions_snapshot) {
          if (context_rlock{*sync_ctx}->dirty.load(std::memory_order_acquire) > 0) {
            needs_render = true;
            break;
          }
        }
      }

      if (renderer_->should_quit())
        running_.store(false, std::memory_order_release);

      // Cap the render rate independent of vsync (which may be unavailable
      // or a no-op on some drivers) so a burst of low-level input events
      // (e.g. a high-poll-rate mouse generating many motion events between
      // sleep_for ticks) can't drive full-tree ImGui rebuilds far above the
      // rate a display could even show. Sessions/input remain marked dirty,
      // so the deferred frame still happens as soon as the interval elapses.
      static constexpr std::chrono::milliseconds kMinFrameInterval{16}; // ~60 FPS
      if (needs_render && std::chrono::steady_clock::now() - last_render_time_ < kMinFrameInterval)
        needs_render = false; // pending_render_ stays set; retried next tick

      // ImGui is immediate-mode: a session's windows only stay on screen if
      // they are resubmitted every frame that gets presented. So we can't
      // selectively skip one session — either the whole frame is drawn (all
      // sessions resubmitted) or none of it is, leaving the previous
      // presented frame on screen unchanged.
      if (needs_render && running_.load(std::memory_order_acquire)) {
        BISON_TRACE_SCOPE("render_frame");
        pending_render_ = false;
        last_render_time_ = std::chrono::steady_clock::now();
        renderer_->begin_frame();
        renderer_->render_server_frame(sessions_snapshot);

        // With zero sessions connected, the per-session loop below (which
        // normally calls service_automation_queries(*sess)) never runs at
        // all -- so a QUERY_TREE sent while no app-under-test has connected
        // yet (or after it disconnected) would never get answered and the
        // browser's getTree() promise would hang forever. Answer with an
        // empty tree instead; see renderer::service_automation_queries()'s
        // no-arg overload doc comment.
        if (sessions_snapshot.empty())
          renderer_->service_automation_queries();

        for (const auto& sync_ctx : sessions_snapshot) {
          std::vector<context::pending_event> events;
          std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> handlers;
          std::function<void(bison::key_t, bison::key_t, bison::dynamic)> client_emit;
          {
            auto sess = context_wlock{*sync_ctx};
            // Decrement dirty before rendering, not after (and not straight
            // to zero): a render function may re-set it (via the const
            // context& it's given) to request further redraws -- e.g. a
            // focused widget animating its own caret, or render_window()
            // confirming a requested modal close needs a couple more
            // frames. Decrementing first, rather than clearing to zero,
            // means that kind of mid-render bump survives this frame's own
            // decrement instead of being immediately stomped by it.
            int32_t d = sess->dirty.load(std::memory_order_relaxed);
            if (d > 0)
              sess->dirty.store(d - 1, std::memory_order_release);
            detail::current_context = &*sess;
            for (const auto& [key, win] : sess->top_level_objects) {
              // MenuBarExtension top-levels are spliced into the server's own
              // chrome menu bar by render_server_frame() above; rendering
              // them again here as standalone windows would double-draw them.
              if (win && win->as<bison::key_t>(bison::dynamic::CLASS) != bison::key_t{"MenuBarExtension"}) {
                sess->current_top_level_key = key;
                renderer_->render_session(*win, *sess);
              }
            }
            sess->current_top_level_key = bison::key_t{};
            detail::current_context = nullptr;
            renderer_->service_automation_queries(*sess);
            events = std::move(sess->pending_events);
            handlers = sess->top_level_handlers;
            client_emit = sess->emit_event;
          }
          // Dispatch events with no lock held: handlers may modify session state.
          for (auto& ev : events) {
            if (client_emit) {
              try {
                client_emit(ev.id, ev.event_name, ev.payload);
              } catch (...) {
              } // client may have disconnected between render and dispatch
            }
            if (ev.root_key.id != 0) {
              auto it = handlers.find(ev.root_key);
              if (it != handlers.end() && it->second)
                it->second->on_event(ev.id, ev.event_name, ev.payload);
            }
          }
          // Handlers dispatched above may have mutated session state (e.g.
          // added a widget in response to a click, or requested a modal
          // close that needs a couple more frames to actually take effect
          // -- see render_window()'s "__request_close__" handling) without
          // any further OS input arriving; force more renders so the
          // change reaches the screen instead of being skipped as idle.
          if (!events.empty())
            context_wlock{*sync_ctx}->dirty.store(kDirtySettleFrames, std::memory_order_release);
        }
        renderer_->end_frame();
        if (!renderer_->render_on_demand() && renderer_->wants_continuous_redraw())
          pending_render_ = true;
        if (renderer_->should_quit())
          running_.store(false, std::memory_order_release);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (renderer_)
    renderer_->teardown();
}

} // namespace bdg::wish
