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
  backend_->on_log([this](const std::string& text, const std::string& level) { handle_log(text, level); });
}

void dbg_source::on_attach_requested(uint32_t pid) {
  attached_ = backend_->attach(pid);
  proxy_
      ->call("set_run_state"_key, payload1("state"_key, std::string{attached_ ? "running" : "detached"}))
      .get();
  if (!attached_)
    return;
  push_breakpoints();
  push_threads();
}

void dbg_source::on_detach_requested() {
  backend_->detach();
  attached_ = false;
  has_selected_thread_ = false;
  has_current_frame_ = false;
  current_stop_file_.clear();
  current_stop_line_ = 0;
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"detached"})).get();
}

void dbg_source::on_pause_requested() {
  backend_->pause();
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"paused"})).get();
  push_threads();
}

void dbg_source::on_resume_requested() {
  backend_->resume();
  current_stop_file_.clear();
  current_stop_line_ = 0;
  has_current_frame_ = false;
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"running"})).get();
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
    push_source(path, current_stop_file_ == path ? current_stop_line_ : 0);
    return;
  }
  if (now_enabled)
    backend_->set_breakpoint(path, line);
  else
    backend_->clear_breakpoint(path, line);
  push_breakpoints();
  push_source(path, current_stop_file_ == path ? current_stop_line_ : 0);
}

void dbg_source::on_select_thread_requested(uint32_t thread_id) {
  selected_thread_id_ = thread_id;
  has_selected_thread_ = true;
  push_callstack(thread_id);
}

void dbg_source::on_select_frame_requested(uint32_t frame_id) {
  current_frame_id_ = frame_id;
  has_current_frame_ = true;
  push_watch(frame_id);
  // DESIGN.md §4's "Thread/frame selection" flow: selecting a frame also
  // opens/focuses that frame's file at its line in the Source window, not
  // just the Watch table.
  for (auto& f : last_frames_) {
    if (static_cast<uint32_t>(f.index) == frame_id) {
      push_source(f.file, f.line);
      break;
    }
  }
}

void dbg_source::on_open_file_requested(const std::string& path, int32_t line) {
  push_source(path, line);
}

void dbg_source::on_add_watch_requested(const std::string& expr) {
  watch_exprs_.push_back(expr);
  // Re-evaluate against whichever frame the Watch window is currently
  // showing (the innermost frame after a stop, or a Call Stack row the
  // user explicitly clicked) so the new expression appears immediately
  // instead of waiting for the next stop/selection. Always push, even with
  // no current frame (e.g. added before the first stop) -- backend_->
  // evaluate() just returns no entries for it yet, but the round trip must
  // still happen or the row silently never appears at all once a frame
  // *is* available, since nothing else re-sends watch_exprs_ on its own.
  push_watch(has_current_frame_ ? current_frame_id_ : 0);
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
  proxy_->call("update_threads"_key, std::move(args)).get();
}

void dbg_source::push_callstack(uint32_t thread_id) {
  auto frames = backend_->get_callstack(thread_id);
  last_frames_ = frames;
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
  proxy_->call("update_callstack"_key, std::move(args)).get();
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
  proxy_->call("update_watch"_key, std::move(args)).get();
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
  proxy_->call("update_breakpoints"_key, std::move(args)).get();
}

void dbg_source::push_source(const std::string& file, int32_t line) {
  dynamic args;
  args["path"_key] = file;
  args["current_line"_key] = line;
  std::vector<int32_t> lines;
  for (auto& bp : breakpoints_) {
    if (bp.file == file && bp.enabled)
      lines.push_back(bp.line);
  }
  args["breakpoint_lines"_key] = std::move(lines);
  proxy_->call("update_source"_key, std::move(args)).get();
}

void dbg_source::handle_stop(const stop_event& ev) {
  selected_thread_id_ = ev.thread_id;
  has_selected_thread_ = true;
  current_stop_file_ = ev.file;
  current_stop_line_ = ev.line;
  current_frame_id_ = 0;
  has_current_frame_ = true;
  proxy_->call("set_run_state"_key, payload1("state"_key, std::string{"paused"})).get();
  push_threads();
  push_callstack(ev.thread_id);
  push_source(ev.file, ev.line);
  // DESIGN.md §4's stop flow re-evaluates the Watch table against the
  // innermost frame on every stop, not just an explicit frame selection.
  push_watch(0);

  dynamic out_args;
  // resolve_address() can legitimately find no line info for some stop
  // addresses (e.g. a loader thunk on attach, or a system DLL with no PDB)
  // -- fall back to just the reason instead of printing the misleading
  // "<reason> at :0" that an empty ev.file/zero ev.line would otherwise
  // produce.
  out_args["text"_key] =
      ev.file.empty() ? ev.reason : ev.reason + " at " + ev.file + ":" + std::to_string(ev.line);
  out_args["level"_key] = std::string{ev.reason == "exception" ? "error" : "info"};
  proxy_->call("append_output"_key, std::move(out_args)).get();
}

void dbg_source::handle_log(const std::string& text, const std::string& level) {
  dynamic out_args;
  out_args["text"_key] = text;
  out_args["level"_key] = level;
  proxy_->call("append_output"_key, std::move(out_args)).get();
}

} // namespace bdg::wish::dbg
