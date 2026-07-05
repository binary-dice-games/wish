// MIT License © 2025 Binary Dice Games
/// @file server.cpp
/// @brief Implementation of wish::server.
#include <file_service.hpp>
#include <forms/form.hpp>
#include <logger.hpp>
#include <registry.hpp>
#include <server.hpp>
#include <style_service.hpp>
#include <ui_root.hpp>

#include "template_handler.hpp"

#include <chrono>
#include <iomanip>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace bdg::wish {

// Thread-local session pointer set by on_before_dispatch and cleared by
// on_after_dispatch.  Form and template-handler code reads session data
// through this pointer so they never re-acquire the already-held wlock.
thread_local session* detail::current_session = nullptr;

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

void server::on_session_created(bison::rmi::context& ctx) {
  auto sync_sess = std::make_shared<sync_session>(std::in_place, ctx.session_id);
  {
    auto sess = sync_sess->wlock();
    sess->emit_event = ctx.emit_event;
    sess->allow_absolute_paths = allow_absolute_paths_;
    sess->file_service = file_service::instantiate(sess->resource_dir);
    sess->style_service = style_service::instantiate();
    // All sessions share the same global logger instance (set via set_logger()).
    sess->logger_service = logger_;
  }
  sessions_.wlock()->emplace(ctx.session_id.id, sync_sess);
  {
    std::ostringstream oss;
    oss << "[rmi] connect     sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
    on_print(ctx.session_id, oss.str());
  }
  auto sess = sync_sess->wlock();
  on_session_created(*sess);
}

void server::on_session_destroyed(bison::rmi::context& ctx) {
  sync_session_ptr sync_sess;
  {
    auto lp = sessions_.wlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end()) {
      sync_sess = it->second;
      lp->erase(it);
    }
  }
  if (sync_sess) {
    try {
      {
        std::ostringstream oss;
        oss << "[rmi] disconnect  sid=0x" << std::hex << std::setw(8) << std::setfill('0') << ctx.session_id.id;
        on_print(ctx.session_id, oss.str());
      }
      auto lock = sync_sess->wlock();
      on_session_destroyed(*lock);
    } catch (...) {
    }
  }
}

bison::dynamic_ptr server::on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) {
  // on_create_object runs inside a dispatch (on_before_dispatch has run), so
  // detail::current_session is set.  We also need the sync_session_ptr for
  // lifetime management when handing it to new objects.
  sync_session_ptr sync_sess;
  {
    auto lp = sessions_.rlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end())
      sync_sess = it->second;
  }

  if (sync_sess && detail::current_session) {
    if (auto svc = detail::find_singleton_service(*detail::current_session, klass))
      return svc;
  }

  // For all other classes, bison creates the concrete type from the registered
  // prototype.  Inject session context into template_handler and form instances.
  auto obj = bison::rmi::server::on_create_object(ctx, ns, klass);
  detail::init_session_object(obj, ctx, sync_sess);
  return obj;
}

void server::on_print(bison::key_t /*session_id*/, const std::string& line) {
  if (logger_)
    logger_->info(line);
}

// Thread-local per-session write lock held for the duration of each dispatch.
// Acquiring it blocks the render thread's per-session rlock, serialising RMI
// handlers against rendering for the same session without blocking other sessions.
thread_local std::optional<sync_session_wlock> tl_dispatch_wlock;

void server::on_before_dispatch(bison::rmi::context& ctx) {
  sync_session_ptr sync_sess;
  {
    auto lp = sessions_.rlock();
    auto it = lp->find(ctx.session_id.id);
    if (it != lp->end())
      sync_sess = it->second;
  }
  if (sync_sess) {
    tl_dispatch_wlock = sync_sess->wlock();
    detail::current_session = &(**tl_dispatch_wlock);
  }
}

void server::on_after_dispatch(bison::rmi::context&) noexcept {
  detail::current_session = nullptr;
  tl_dispatch_wlock.reset();
}

void server::render_loop() {
  if (renderer_)
    renderer_->setup();
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      renderer_->begin_frame();
      renderer_->render_server_frame();
      {
        // Snapshot the set of sync_session pointers under a brief sessions_
        // rlock, then render each session under its own per-session rlock.
        // This lets different sessions render without blocking each other
        // and keeps sessions_ unblocked for session lifecycle operations.
        std::vector<sync_session_ptr> to_render;
        {
          auto lp = sessions_.rlock();
          to_render.reserve(lp->size());
          for (const auto& [id, sp] : *lp)
            to_render.push_back(sp);
        }
        for (const auto& sync_sess : to_render) {
          std::vector<session::pending_event> events;
          std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> handlers;
          std::function<void(bison::key_t, bison::key_t, bison::dynamic)> client_emit;
          {
            auto sess = sync_sess->wlock();
            detail::current_session = &*sess;
            for (const auto& [key, win] : sess->top_level_objects) {
              if (win) {
                sess->current_top_level_key = key;
                renderer_->render_session(*win, *sess);
              }
            }
            sess->current_top_level_key = bison::key_t{};
            detail::current_session = nullptr;
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
        }
      }
      renderer_->end_frame();
      if (renderer_->should_quit())
        running_.store(false, std::memory_order_release);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (renderer_)
    renderer_->teardown();
}

} // namespace bdg::wish
