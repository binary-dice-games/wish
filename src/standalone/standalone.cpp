// MIT License © 2025 Binary Dice Games
/// @file standalone.cpp
/// @brief Implementation of wish::standalone.
#include <standalone/standalone.hpp>

#include <context/file_service.hpp>
#include <server/registry.hpp>
#include <context/style_service.hpp>
#include <ui/ui_descriptor.hpp>
#include <ui/ui_root.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {
// Thread-local per-dispatch write lock -- distinct symbol from server.cpp's
// equivalent (different translation unit, same purpose: serialize RMI
// handlers against rendering without blocking other standalone instances).
thread_local std::optional<context_wlock> tl_standalone_dispatch_wlock;
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
  // render thread starts -- so context_ is fully published (via the
  // happens-before edge of std::thread's own construction) with no need to
  // separately synchronize access to the context_ member itself.
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
  context_ = std::make_shared<sync_context>(std::in_place, on_create_context(ctx.session_id));
  {
    auto sess = context_wlock{*context_};
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
  if (!context_)
    return;
  auto lock = context_wlock{*context_};
  on_session_destroyed(*lock);
}

bison::dynamic_ptr standalone::on_create_object(bison::rmi::context& ctx, bison::key_t ns, bison::key_t klass) {
  if (context_ && detail::current_context) {
    if (auto svc = detail::find_singleton_service(*detail::current_context, klass))
      return svc;
  }

  auto obj = bison::rmi::standalone::on_create_object(ctx, ns, klass);
  detail::init_session_object(obj, ctx, context_);
  return obj;
}

void standalone::on_before_dispatch(bison::rmi::context& /*ctx*/) {
  if (!context_)
    return;
  tl_standalone_dispatch_wlock.emplace(*context_);
  detail::current_context = &**tl_standalone_dispatch_wlock;
}

void standalone::on_after_dispatch(bison::rmi::context& /*ctx*/) noexcept {
  // Any dispatched RMI call may have mutated session state (properties,
  // tree structure, style); flag the session so the render loop redraws it
  // instead of skipping the next idle-check.
  if (tl_standalone_dispatch_wlock)
    (*tl_standalone_dispatch_wlock)->dirty.store(kDirtySettleFrames, std::memory_order_release);
  detail::current_context = nullptr;
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
      if (renderer_->poll_events())
        pending_render_ = true;
      bool needs_render = pending_render_;
      if (!needs_render && context_)
        needs_render = context_rlock{*context_}->dirty.load(std::memory_order_acquire) > 0;

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

      if (needs_render && running_.load(std::memory_order_acquire)) {
        pending_render_ = false;
        last_render_time_ = std::chrono::steady_clock::now();
        renderer_->begin_frame();
        // standalone embeds at most one session, so the "sessions" list
        // render_server_frame() sees for chrome-extension purposes is either
        // empty or a single element.
        std::vector<sync_context_ptr> sessions_snapshot;
        if (context_)
          sessions_snapshot.push_back(context_);
        renderer_->render_server_frame(sessions_snapshot);
        // See the matching comment in wish::server::render_loop(): with no
        // embedded session, service_automation_queries(*sess) below never
        // runs, so a QUERY_TREE would hang forever without this.
        if (!context_)
          renderer_->service_automation_queries();
        if (context_) {
          std::vector<context::pending_event> events;
          std::unordered_map<bison::key_t, ui_root*, bison::key_t, bison::key_t> handlers;
          std::function<void(bison::key_t, bison::key_t, bison::dynamic)> client_emit;
          {
            auto sess = context_wlock{*context_};
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
              if (win) {
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
              } // caller may have torn things down between render and dispatch
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
            context_wlock{*context_}->dirty.store(kDirtySettleFrames, std::memory_order_release);
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

// ── Wish-level convenience helpers (mirror wish::client) ──────────────────────

std::future<void> standalone::register_template(bison::key_t name, bison::dynamic descriptor) {
  return std::async(std::launch::async, [this, name, d = std::move(descriptor)]() mutable {
    dynamic args;
    args["name"_key] = name;
    args["descriptor"_key] = dynamic_ptr{std::move(d)};
    template_proxy_->call("register"_key, std::move(args)).get();
  });
}

std::future<void> standalone::register_template_from_json(bison::key_t name, const std::string& json) {
  return register_template(name, import_descriptor_json(json));
}

std::future<void> standalone::register_template_from_yaml(bison::key_t name, const std::string& yaml) {
  return register_template(name, import_descriptor_yaml(yaml));
}

namespace {
constexpr std::size_t kStandaloneFileChunkSize = 1u << 20; // 1 MiB
} // namespace

std::future<void>
standalone::upload_file(const std::string& name, const std::string& data, transfer_progress_callback on_progress) {
  if (!on_progress) {
    return std::async(std::launch::async, [this, name, data]() {
      dynamic args;
      args["name"_key] = name;
      args["data"_key] = data;
      fs_proxy_->call("upload"_key, std::move(args)).get();
    });
  }
  return std::async(std::launch::async, [this, name, data, on_progress]() {
    std::uint64_t total = data.size();
    std::uint64_t sent = 0;
    bool first = true;
    for (std::size_t pos = 0; pos == 0 || pos < data.size(); pos += kStandaloneFileChunkSize) {
      auto n = std::min<std::size_t>(kStandaloneFileChunkSize, data.size() - pos);
      bool eof = pos + n >= data.size();

      dynamic args;
      args["name"_key] = name;
      args["data"_key] = data.substr(pos, n);
      args["first"_key] = first;
      args["eof"_key] = eof;
      fs_proxy_->call("upload_chunk"_key, std::move(args)).get();

      sent += n;
      if (on_progress)
        on_progress(sent, total);

      first = false;
      if (eof)
        break;
    }
  });
}

std::future<std::string> standalone::download_file(const std::string& name, transfer_progress_callback on_progress) {
  if (!on_progress) {
    return std::async(std::launch::async, [this, name]() -> std::string {
      dynamic args;
      args["name"_key] = name;
      auto result = fs_proxy_->call("download"_key, std::move(args)).get();
      return result.as<std::string>("result"_key);
    });
  }
  return std::async(std::launch::async, [this, name, on_progress]() -> std::string {
    std::string result_data;
    int32_t offset = 0;
    for (;;) {
      dynamic args;
      args["name"_key] = name;
      args["offset"_key] = offset;
      args["max_size"_key] = static_cast<int32_t>(kStandaloneFileChunkSize);
      auto result = fs_proxy_->call("download_chunk"_key, std::move(args)).get();

      auto chunk = result.as<std::string>("data"_key);
      bool eof = result.as<bool>("eof"_key);
      auto total = static_cast<std::uint64_t>(result.as<int32_t>("total"_key));
      if (!chunk.empty()) {
        result_data.append(chunk);
        offset += static_cast<int32_t>(chunk.size());
      }
      if (on_progress)
        on_progress(static_cast<std::uint64_t>(offset), total);
      if (eof)
        break;
    }
    return result_data;
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
