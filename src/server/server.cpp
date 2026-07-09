// MIT License © 2025 Binary Dice Games
/// @file server.cpp
/// @brief Implementation of wish::server.
#include <context/file_service.hpp>
#include <ui/forms/form.hpp>
#include <context/logger.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/style_service.hpp>
#include <ui/ui_root.hpp>

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

void server::start() {
  register_all();
  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this]() { render_loop(); });
  listen();
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
  s.file_service = file_service::instantiate(s.resource_dir);
  s.style_service = style_service::instantiate();
  // All sessions share the same global logger instance (set via set_logger()).
  s.logger_service = logger_;
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
  static_cast<context&>(ctx).dirty.store(true, std::memory_order_release);
  detail::current_context = nullptr;
}

void server::render_loop() {
  if (renderer_)
    renderer_->setup();
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      // poll_events() must run every iteration regardless of whether a
      // frame is drawn, so OS event queues are drained and window-close
      // requests are never delayed by an idle skip below.
      if (renderer_->poll_events())
        pending_render_ = true;
      bool needs_render = pending_render_;

      // Snapshot the set of sync_context pointers under a brief
      // session_contexts() rlock; reused below both for the cheap dirty scan
      // and (if needed) for rendering. This lets different sessions render
      // without blocking each other and keeps session_contexts() unblocked
      // for session lifecycle operations.
      std::vector<sync_context_ptr> sessions_snapshot;
      {
        auto lp = session_contexts().rlock();
        sessions_snapshot.reserve(lp->size());
        for (const auto& [id, sp] : *lp)
          sessions_snapshot.push_back(sp);
      }

      if (!needs_render) {
        for (const auto& sync_ctx : sessions_snapshot) {
          if (context_rlock{*sync_ctx}->dirty.load(std::memory_order_acquire)) {
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
        pending_render_ = false;
        last_render_time_ = std::chrono::steady_clock::now();
        renderer_->begin_frame();
        renderer_->render_server_frame();
        for (const auto& sync_ctx : sessions_snapshot) {
          std::vector<context::pending_event> events;
          std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> handlers;
          std::function<void(bison::key_t, bison::key_t, bison::dynamic)> client_emit;
          {
            auto sess = context_wlock{*sync_ctx};
            // Clear dirty before rendering, not after: a render function may
            // re-set it (via the const context& it's given) to request
            // continuous redraws, e.g. a focused widget animating its own
            // caret. Clearing afterward would stomp that signal.
            sess->dirty.store(false, std::memory_order_release);
            detail::current_context = &*sess;
            for (const auto& [key, win] : sess->top_level_objects) {
              if (win) {
                sess->current_top_level_key = key;
                renderer_->render_session(*win, *sess);
              }
            }
            sess->current_top_level_key = bison::key_t{};
            detail::current_context = nullptr;
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
          // added a widget in response to a click) without any further OS
          // input arriving; force one more render so the change reaches the
          // screen instead of being skipped as idle.
          if (!events.empty())
            context_wlock{*sync_ctx}->dirty.store(true, std::memory_order_release);
        }
        renderer_->end_frame();
        if (renderer_->wants_continuous_redraw())
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
