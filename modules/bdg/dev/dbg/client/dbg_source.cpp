// MIT License © 2026 Binary Dice Games
/// @file dbg_source.cpp
/// @brief Implementation of dbg_source.
#include "dbg_source.hpp"

namespace bdg::wish::dbg {

using namespace bison;

namespace {
template <typename T>
dynamic payload1(key_t k, T v) {
  dynamic d;
  d[k] = std::move(v);
  return d;
}
template <typename T1, typename T2>
dynamic payload2(key_t k1, T1 v1, key_t k2, T2 v2) {
  dynamic d;
  d[k1] = std::move(v1);
  d[k2] = std::move(v2);
  return d;
}
} // namespace

dbg_source::dbg_source(std::shared_ptr<rmi::proxy::dynamic> proxy, std::unique_ptr<debug_backend> backend)
    : proxy_(std::move(proxy)), backend_(std::move(backend)) {
  backend_->on_stop([this](const stop_event& ev) { handle_stop(ev); });
}

void dbg_source::on_attach_requested(uint32_t pid) {
  attached_ = backend_->attach(pid);
  proxy_->call(
      "set_run_state"_key, payload1("state"_key, std::string{attached_ ? "running" : "detached"}));
  if (!attached_)
    return;
  push_breakpoints();
  push_threads();
}

void dbg_source::on_detach_requested() {
  backend_->detach();
  attached_ = false;
  has_selected_thread_ = false;
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"detached"}));
}

void dbg_source::on_pause_requested() {
  backend_->pause();
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"paused"}));
  push_threads();
}

void dbg_source::on_resume_requested() {
  backend_->resume();
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"running"}));
}

void dbg_source::on_step_requested(const std::string& kind, uint32_t thread_id) {
  if (kind == "into")
    backend_->step_into(thread_id);
  else if (kind == "over")
    backend_->step_over(thread_id);
  else if (kind == "out")
    backend_->step_out(thread_id);
}

void dbg_source::on_toggle_breakpoint_requested(const std::string& path, int32_t line) {
  bool now_enabled = true;
  bool found = false;
  for (auto& bp : breakpoints_) {
    if (bp.file == path && bp.line == line) {
      bp.enabled = !bp.enabled;
      now_enabled = bp.enabled;
      found = true;
      break;
    }
  }
  if (!found) {
    breakpoints_.push_back({path, line, true});
  } else if (!now_enabled) {
    backend_->clear_breakpoint(path, line);
    push_breakpoints();
    return;
  }
  if (now_enabled)
    backend_->set_breakpoint(path, line);
  else
    backend_->clear_breakpoint(path, line);
  push_breakpoints();
}

void dbg_source::on_select_thread_requested(uint32_t thread_id) {
  selected_thread_id_ = thread_id;
  has_selected_thread_ = true;
  push_callstack(thread_id);
}

void dbg_source::on_select_frame_requested(uint32_t frame_id) {
  push_watch(frame_id);
}

void dbg_source::on_add_watch_requested(const std::string& expr) {
  watch_exprs_.push_back(expr);
  if (has_selected_thread_) {
    // Re-evaluate against frame 0 of the currently-selected thread's stack
    // -- the Watch window always shows the innermost frame's view unless
    // the user has explicitly selected a different Call Stack row (tracked
    // server-side; this client doesn't need to know which frame index was
    // last clicked, only push_watch(frame_id) does).
  }
}

void dbg_source::push_threads() {
  auto threads = backend_->get_threads();
  dynamic args;
  dynamic_ptr arr{key_t{0U}, {}};
  size_t i = 0;
  for (auto& t : threads) {
    dynamic entry;
    entry["id"_key] = static_cast<int32_t>(t.id);
    entry["state"_key] = t.state;
    entry["current_function"_key] = t.current_function;
    (*arr)[i++] = dynamic_ptr{std::make_shared<dynamic>(std::move(entry))};
  }
  args["threads"_key] = arr;
  proxy_->call("update_threads"_key, args);
}

void dbg_source::push_callstack(uint32_t thread_id) {
  auto frames = backend_->get_callstack(thread_id);
  dynamic args;
  args["thread_id"_key] = static_cast<int32_t>(thread_id);
  dynamic_ptr arr{key_t{0U}, {}};
  size_t i = 0;
  for (auto& f : frames) {
    dynamic entry;
    entry["index"_key] = f.index;
    entry["function"_key] = f.function;
    entry["file"_key] = f.file;
    entry["line"_key] = f.line;
    (*arr)[i++] = dynamic_ptr{std::make_shared<dynamic>(std::move(entry))};
  }
  args["frames"_key] = arr;
  proxy_->call("update_callstack"_key, args);
}

void dbg_source::push_watch(uint32_t frame_id) {
  auto entries = backend_->evaluate(frame_id, watch_exprs_);
  dynamic args;
  args["frame_id"_key] = static_cast<int32_t>(frame_id);
  dynamic_ptr arr{key_t{0U}, {}};
  size_t i = 0;
  for (auto& e : entries) {
    dynamic entry;
    entry["name"_key] = e.name;
    entry["value"_key] = e.value;
    entry["type"_key] = e.type;
    (*arr)[i++] = dynamic_ptr{std::make_shared<dynamic>(std::move(entry))};
  }
  args["entries"_key] = arr;
  proxy_->call("update_watch"_key, args);
}

void dbg_source::push_breakpoints() {
  dynamic args;
  dynamic_ptr arr{key_t{0U}, {}};
  size_t i = 0;
  for (auto& bp : breakpoints_) {
    dynamic entry;
    entry["file"_key] = bp.file;
    entry["line"_key] = bp.line;
    entry["enabled"_key] = bp.enabled;
    (*arr)[i++] = dynamic_ptr{std::make_shared<dynamic>(std::move(entry))};
  }
  args["breakpoints"_key] = arr;
  proxy_->call("update_breakpoints"_key, args);
}

void dbg_source::handle_stop(const stop_event& ev) {
  selected_thread_id_ = ev.thread_id;
  has_selected_thread_ = true;
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"paused"}));
  push_threads();
  push_callstack(ev.thread_id);

  dynamic out_args;
  out_args["text"_key] = ev.reason + " at " + ev.file + ":" + std::to_string(ev.line);
  out_args["level"_key] = std::string{"info"};
  proxy_->call("append_output"_key, out_args);
}

} // namespace bdg::wish::dbg
