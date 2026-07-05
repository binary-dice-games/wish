// MIT License © 2025 Binary Dice Games
/// @file standalone.cpp
/// @brief Implementation of wish::standalone.
#include <standalone.hpp>

#include <file_service.hpp>
#include <registry.hpp>
#include <style_service.hpp>
#include <ui_root.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {
// Thread-local per-dispatch write lock -- distinct symbol from server.cpp's
// tl_dispatch_wlock (different translation unit, same purpose: serialize RMI
// handlers against rendering without blocking other standalone instances).
thread_local std::optional<sync_session_wlock> tl_standalone_dispatch_wlock;
} // namespace

// ── standalone ────────────────────────────────────────────────────────────────

standalone::standalone(std::unique_ptr<renderer> r) : renderer_(std::move(r)) {}

standalone::~standalone() {
  if (running_.load(std::memory_order_acquire)) {
    try {
      stop();
    } catch (...) {
    }
  }
}

void standalone::start() {
  register_all();
  // Fires on_session_created() synchronously on this thread, before the
  // render thread starts -- so session_ is fully published (via the
  // happens-before edge of std::thread's own construction) with no need to
  // separately synchronize access to the session_ member itself.
  connect();
  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread(&standalone::render_loop, this);
}

void standalone::stop() {
  running_.store(false, std::memory_order_release);
  disconnect(); // fires on_session_destroyed(), then stops the worker thread
  if (render_thread_.joinable())
    render_thread_.join();
}

bool standalone::should_quit() const {
  return !running_.load(std::memory_order_acquire);
}

void standalone::on_session_created(bison::rmi::context& ctx) {
  session_ = std::make_shared<sync_session>(std::in_place, ctx.session_id);
  {
    auto sess = session_->wlock();
    sess->emit_event = ctx.emit_event;
    sess->allow_absolute_paths = allow_absolute_paths_;
    sess->file_service = file_service::instantiate(sess->resource_dir);
    sess->style_service = style_service::instantiate();
    sess->logger_service = logger_;
    on_session_created(*sess);
  }

  template_proxy_ = instantiate("wish"_key, "__WishTemplate"_key).get();
  fs_proxy_ = instantiate("wish"_key, "__WishFileSystem"_key).get();
  style_proxy_ = instantiate("wish"_key, "__WishStyle"_key).get();
  log_proxy_ = instantiate("wish"_key, "__WishLogger"_key).get();
}

void standalone::on_session_destroyed(bison::rmi::context& ctx) {
  (void)ctx;
  if (!session_)
    return;
  auto lock = session_->wlock();
  on_session_destroyed(*lock);
}

bison::dynamic_ptr standalone::on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) {
  if (session_ && detail::current_session) {
    if (auto svc = detail::find_singleton_service(*detail::current_session, klass))
      return svc;
  }

  auto obj = bison::rmi::standalone::on_create_object(ctx, ns, klass);
  detail::init_session_object(obj, ctx, session_);
  return obj;
}

void standalone::on_before_dispatch(bison::rmi::context& /*ctx*/) {
  if (!session_)
    return;
  tl_standalone_dispatch_wlock = session_->wlock();
  detail::current_session = &(**tl_standalone_dispatch_wlock);
}

void standalone::on_after_dispatch(bison::rmi::context& /*ctx*/) noexcept {
  // Any dispatched RMI call may have mutated session state (properties,
  // tree structure, style); flag the session so the render loop redraws it
  // instead of skipping the next idle-check.
  if (tl_standalone_dispatch_wlock)
    (*tl_standalone_dispatch_wlock)->dirty.store(true, std::memory_order_release);
  detail::current_session = nullptr;
  tl_standalone_dispatch_wlock.reset();
}

void standalone::render_loop() {
  if (renderer_)
    renderer_->setup();
  while (running_.load(std::memory_order_acquire)) {
    if (renderer_) {
      // poll_events() must run every iteration regardless of whether a
      // frame is drawn, so OS event queues are drained and window-close
      // requests are never delayed by an idle skip below.
      bool needs_render = renderer_->poll_events();
      if (!needs_render && session_)
        needs_render = session_->rlock()->dirty.load(std::memory_order_acquire);

      if (renderer_->should_quit())
        running_.store(false, std::memory_order_release);

      if (needs_render && running_.load(std::memory_order_acquire)) {
        renderer_->begin_frame();
        renderer_->render_server_frame();
        if (session_) {
          std::vector<session::pending_event> events;
          std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> handlers;
          std::function<void(bison::key_t, bison::key_t, bison::dynamic)> client_emit;
          {
            auto sess = session_->wlock();
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
            sess->dirty.store(false, std::memory_order_release);
          }
          // Dispatch events with no lock held: handlers may modify session state.
          for (auto& ev : events) {
            if (client_emit) {
              try {
                client_emit(ev.id, ev.event_name, ev.payload);
              } catch (...) {
              } // caller may have torn things down between render and dispatch
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
            session_->wlock()->dirty.store(true, std::memory_order_release);
        }
        renderer_->end_frame();
        if (renderer_->should_quit())
          running_.store(false, std::memory_order_release);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (renderer_)
    renderer_->teardown();
}

// ── Wish-level convenience helpers (mirror wish::client) ──────────────────────

std::future<void> standalone::register_template(bison::key_t name, const std::string& descriptor) {
  return std::async(std::launch::async, [this, name, descriptor]() {
    dynamic args;
    args["name"_key] = name;
    args["descriptor"_key] = descriptor;
    template_proxy_->call("register"_key, std::move(args)).get();
  });
}

std::future<void> standalone::upload_file(const std::string& name, const std::string& data) {
  return std::async(std::launch::async, [this, name, data]() {
    dynamic args;
    args["name"_key] = name;
    args["data"_key] = data;
    fs_proxy_->call("upload"_key, std::move(args)).get();
  });
}

std::future<std::string> standalone::download_file(const std::string& name) {
  return std::async(std::launch::async, [this, name]() -> std::string {
    dynamic args;
    args["name"_key] = name;
    auto result = fs_proxy_->call("download"_key, std::move(args)).get();
    return result.as<std::string>("result"_key);
  });
}

std::future<void> standalone::set_style_preset(const std::string& name) {
  return std::async(std::launch::async, [this, name]() {
    dynamic args;
    args["name"_key] = name;
    // oneway=true: same reasoning as wish::client::set_style_preset -- lets
    // the call be made safely from within event callbacks on the render
    // thread, which would otherwise deadlock waiting for its own response.
    style_proxy_->call("preset"_key, std::move(args), true).get();
  });
}

std::future<void> standalone::set_style(bison::dynamic params) {
  return std::async(std::launch::async, [this, p = std::move(params)]() mutable {
    style_proxy_->call("set"_key, std::move(p), true).get();
  });
}

std::future<bison::dynamic> standalone::get_style() {
  return std::async(std::launch::async, [this]() -> dynamic { return style_proxy_->call("get"_key, dynamic{}).get(); });
}

std::future<void> standalone::log(const std::string& level, const std::string& msg) {
  return std::async(std::launch::async, [this, level, msg]() {
    dynamic args;
    args["level"_key] = level;
    args["msg"_key] = msg;
    // oneway=true: log messages are fire-and-forget to avoid blocking callers.
    log_proxy_->call("log"_key, std::move(args), true).get();
  });
}

std::future<void> standalone::log_debug(const std::string& msg) {
  return log("debug", msg);
}
std::future<void> standalone::log_info(const std::string& msg) {
  return log("info", msg);
}
std::future<void> standalone::log_warn(const std::string& msg) {
  return log("warn", msg);
}
std::future<void> standalone::log_error(const std::string& msg) {
  return log("error", msg);
}

} // namespace bdg::wish
