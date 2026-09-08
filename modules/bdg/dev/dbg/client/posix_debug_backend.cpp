// MIT License © 2026 Binary Dice Games
/// @file posix_debug_backend.cpp
/// @brief Implementation of posix_debug_backend -- drives a child
///        `gdb --interpreter=mi` (or `lldb-mi`) process over its MI
///        protocol. See the header for the threading model and DESIGN.md
///        §3 for where this fits the module.
#include "posix_debug_backend.hpp"

#if !defined(_WIN32)

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace bdg::wish::dbg {

namespace {

/// @brief Blocking write of the whole buffer; false on any error.
bool write_all(int fd, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

/// @brief `basename`-contains test used to pick MI launch arguments.
bool name_contains(const std::string& path, const char* needle) {
  auto slash = path.find_last_of('/');
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  return base.find(needle) != std::string::npos;
}

bool is_executable(const std::string& path) {
  return ::access(path.c_str(), X_OK) == 0;
}

/// @brief First `name` found on `$PATH`, or "".
std::string which(const char* name) {
  const char* path_env = ::getenv("PATH");
  if (!path_env)
    return {};
  std::string path = path_env;
  size_t start = 0;
  while (start <= path.size()) {
    size_t colon = path.find(':', start);
    std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty()) {
      std::string candidate = dir + "/" + name;
      if (is_executable(candidate))
        return candidate;
    }
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  return {};
}

int to_int(const std::string& s) {
  return s.empty() ? 0 : static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
}

/// @brief `--thread N ` prefix for MI general options, or "" when @p tid is 0.
std::string thread_opt(uint32_t tid) {
  return tid ? " --thread " + std::to_string(tid) : std::string();
}

} // namespace

// ── Construction / teardown ───────────────────────────────────────────────

posix_debug_backend::~posix_debug_backend() {
  detach();
}

std::string posix_debug_backend::discover_debugger() {
  if (const char* forced = ::getenv("WISH_DBG_DEBUGGER"); forced && *forced) {
    // Accept either an absolute path or a bare name to resolve on PATH.
    if (std::strchr(forced, '/'))
      return is_executable(forced) ? std::string(forced) : std::string();
    return which(forced);
  }
  if (std::string gdb = which("gdb"); !gdb.empty())
    return gdb;
  if (std::string lldb_mi = which("lldb-mi"); !lldb_mi.empty())
    return lldb_mi;
  return {};
}

bool posix_debug_backend::spawn_debugger() {
  // Writing to a debugger that has already exited must not raise SIGPIPE
  // and kill the whole wish client -- report the failed write instead.
  static std::once_flag sigpipe_once;
  std::call_once(sigpipe_once, [] { ::signal(SIGPIPE, SIG_IGN); });

  int in_pipe[2];  // parent writes [1] -> child stdin [0]
  int out_pipe[2]; // child stdout/stderr [1] -> parent reads [0]
  if (::pipe(in_pipe) != 0)
    return false;
  if (::pipe(out_pipe) != 0) {
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    return false;
  }

  // argv is built before fork() so the child does only async-signal-safe work.
  std::vector<std::string> args;
  args.push_back(debugger_path_);
  if (name_contains(debugger_path_, "lldb-mi")) {
    // lldb-mi *is* the MI interpreter -- no selector flag.
  } else {
    args.push_back("--interpreter=mi");
    args.push_back("--nx"); // Skip ~/.gdbinit so behaviour is reproducible.
    args.push_back("-q");
  }
  std::vector<char*> argv;
  for (auto& a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return false;
  }
  if (pid == 0) {
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(out_pipe[1], STDERR_FILENO);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::execv(argv[0], argv.data());
    _exit(127); // exec failed.
  }

  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  child_pid_ = pid;
  child_stdin_ = in_pipe[1];
  child_stdout_ = out_pipe[0];
  return true;
}

void posix_debug_backend::teardown() {
  if (child_pid_ < 0 && !reader_thread_.joinable() && !dispatch_thread_.joinable())
    return;

  shutting_down_ = true;

  // Ask the debugger to quit, then drop our write end so it sees EOF on
  // stdin even if it ignored the request.
  if (child_stdin_ >= 0) {
    write_all(child_stdin_, "9999-gdb-exit\n");
    ::close(child_stdin_);
    child_stdin_ = -1;
  }

  // Unblock a send_command() that is still waiting for a reply that will
  // never arrive now.
  {
    std::lock_guard<std::mutex> lk(reply_mtx_);
    reader_gone_ = true;
  }
  reply_cv_.notify_all();
  queue_cv_.notify_all();

  // Give the debugger a moment to exit on its own, then make sure.
  if (child_pid_ >= 0) {
    for (int i = 0; i < 20; ++i) {
      int status = 0;
      pid_t r = ::waitpid(child_pid_, &status, WNOHANG);
      if (r == child_pid_ || r < 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      if (i == 10)
        ::kill(child_pid_, SIGKILL);
    }
  }

  if (reader_thread_.joinable())
    reader_thread_.join();
  if (dispatch_thread_.joinable())
    dispatch_thread_.join();

  if (child_stdout_ >= 0) {
    ::close(child_stdout_);
    child_stdout_ = -1;
  }
  if (child_pid_ >= 0) {
    int status = 0;
    ::waitpid(child_pid_, &status, 0);
    child_pid_ = -1;
  }
  shutting_down_ = false;
}

// ── debug_backend: lifecycle ──────────────────────────────────────────────

bool posix_debug_backend::attach(uint32_t pid) {
  if (attached_)
    return false;

  debugger_path_ = discover_debugger();
  if (debugger_path_.empty()) {
    if (auto cb = *on_log_cb_.rlock())
      cb("No debugger found: install gdb (or lldb-mi), or set $WISH_DBG_DEBUGGER.", "error");
    return false;
  }

  reported_attach_ = false;
  reader_gone_ = false;
  running_ = false;
  pause_requested_ = false;
  shutting_down_ = false;

  if (!spawn_debugger()) {
    if (auto cb = *on_log_cb_.rlock())
      cb(std::string("Failed to start debugger '") + debugger_path_ + "'.", "error");
    return false;
  }

  reader_thread_ = std::thread([this] { reader_loop(); });
  dispatch_thread_ = std::thread([this] { dispatch_loop(); });

  // Async MI is required, not optional: in synchronous mode gdb stops
  // reading commands until the target next stops, so `-exec-interrupt`
  // (what pause() needs) would just sit unread while the debuggee runs.
  // All-stop mode (the default) keeps it to one `*stopped` per event
  // rather than one per thread. No pager, no confirmation prompts.
  // lldb-mi rejects some of these -- ignore failures.
  send_command("-gdb-set mi-async on", 3000);
  send_command("-gdb-set non-stop off", 3000);
  send_command("-gdb-set pagination off", 3000);
  send_command("-gdb-set confirm off", 3000);

  command_result r = send_command("-target-attach " + std::to_string(pid), 15000);
  if (!r.ok) {
    std::string msg = r.record.get("msg");
    if (auto cb = *on_log_cb_.rlock())
      cb("Attach to pid " + std::to_string(pid) + " failed" + (msg.empty() ? "." : ": " + msg), "error");
    teardown();
    return false;
  }

  attached_ = true;
  return true;
}

void posix_debug_backend::detach() {
  if (!reader_thread_.joinable() && child_pid_ < 0) {
    attached_ = false;
    return;
  }
  // The target must be stopped for the debugger to detach cleanly.
  if (running_) {
    send_command("-exec-interrupt", 3000);
    for (int i = 0; i < 40 && running_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  send_command("-target-detach", 5000);
  teardown();
  attached_ = false;
}

// ── debug_backend: execution control ──────────────────────────────────────

void posix_debug_backend::pause() {
  if (!attached_)
    return;
  pause_requested_ = true;
  send_command("-exec-interrupt", 3000);
}

void posix_debug_backend::resume() {
  if (!attached_)
    return;
  pause_requested_ = false;
  send_command("-exec-continue");
}

void posix_debug_backend::step_into(uint32_t thread_id) {
  if (attached_)
    send_command("-exec-step" + thread_opt(thread_id));
}

void posix_debug_backend::step_over(uint32_t thread_id) {
  if (attached_)
    send_command("-exec-next" + thread_opt(thread_id));
}

void posix_debug_backend::step_out(uint32_t thread_id) {
  if (attached_)
    send_command("-exec-finish" + thread_opt(thread_id));
}

// ── debug_backend: breakpoints ────────────────────────────────────────────

bool posix_debug_backend::set_breakpoint(const std::string& file, int line) {
  if (!attached_)
    return false;
  std::string key = file + ":" + std::to_string(line);
  command_result r = send_command("-break-insert " + key);
  if (!r.ok)
    return false;
  const mi_value* bkpt = r.record.find("bkpt");
  int number = bkpt ? to_int(bkpt->get("number")) : 0;
  if (number == 0)
    return false;
  (*breakpoints_.wlock())[key] = number;
  return true;
}

void posix_debug_backend::clear_breakpoint(const std::string& file, int line) {
  std::string key = file + ":" + std::to_string(line);
  int number = 0;
  {
    auto wl = breakpoints_.wlock();
    auto it = wl->find(key);
    if (it == wl->end())
      return;
    number = it->second;
    wl->erase(it);
  }
  if (attached_)
    send_command("-break-delete " + std::to_string(number));
}

// ── debug_backend: inspection ─────────────────────────────────────────────

std::vector<thread_info> posix_debug_backend::get_threads() {
  std::vector<thread_info> result;
  if (!attached_)
    return result;
  command_result r = send_command("-thread-info");
  if (!r.ok)
    return result;
  const mi_value* threads = r.record.find("threads");
  if (!threads)
    return result;
  for (const auto& t : threads->values) {
    thread_info info;
    info.id = static_cast<uint32_t>(to_int(t.get("id")));
    info.state = t.get("state") == "running" ? "running" : "suspended";
    if (const mi_value* frame = t.find("frame"))
      info.current_function = frame->get("func");
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<frame_info> posix_debug_backend::get_callstack(uint32_t thread_id) {
  std::vector<frame_info> frames;
  if (!attached_)
    return frames;
  command_result r = send_command("-stack-list-frames" + thread_opt(thread_id));
  if (!r.ok)
    return frames;
  const mi_value* stack = r.record.find("stack");
  if (!stack)
    return frames;
  // GDB renders `stack` as a result-style list: stack=[frame={...},frame={...}].
  for (const auto& [name, frame] : stack->items) {
    if (name != "frame")
      continue;
    frame_info fi;
    fi.index = to_int(frame.get("level"));
    fi.function = frame.get("func");
    std::string full = frame.get("fullname");
    fi.file = full.empty() ? frame.get("file") : full;
    fi.line = to_int(frame.get("line"));
    frames.push_back(std::move(fi));
  }
  return frames;
}

std::vector<watch_entry> posix_debug_backend::evaluate(uint32_t frame_id,
                                                       const std::vector<std::string>& exprs) {
  std::vector<watch_entry> result;
  if (!attached_ || exprs.empty())
    return result;
  uint32_t tid = last_stop_thread_.load();
  if (tid == 0)
    tid = 1;
  std::string scope = " --thread " + std::to_string(tid) + " --frame " + std::to_string(frame_id);

  for (const auto& expr : exprs) {
    command_result v = send_command("-data-evaluate-expression" + scope + " \"" + expr + "\"");
    if (!v.ok)
      continue;
    watch_entry entry;
    entry.name = expr;
    entry.value = v.record.get("value");

    // MI has no "give me the type" result field for a raw expression; the
    // CLI `whatis` command does, echoed as a `~"type = ...\n"` stream line
    // (DESIGN.md §1's Watch scope -- simple local reads, not full
    // expression evaluation).
    command_result t = send_command("-interpreter-exec" + scope + " console \"whatis " + expr + "\"");
    const std::string kNeedle = "type = ";
    auto p = t.streams.find(kNeedle);
    if (p != std::string::npos) {
      std::string type = t.streams.substr(p + kNeedle.size());
      auto nl = type.find_first_of("\r\n");
      if (nl != std::string::npos)
        type.erase(nl);
      entry.type = type;
    }
    result.push_back(std::move(entry));
  }
  return result;
}

// ── debug_backend: callbacks ──────────────────────────────────────────────

void posix_debug_backend::on_stop(stop_callback cb) {
  *on_stop_cb_.wlock() = std::move(cb);
}

void posix_debug_backend::on_log(log_callback cb) {
  *on_log_cb_.wlock() = std::move(cb);
}

// ── MI command handshake ──────────────────────────────────────────────────

posix_debug_backend::command_result posix_debug_backend::send_command(const std::string& command,
                                                                      int timeout_ms) {
  std::lock_guard<std::mutex> send_lk(send_mtx_);
  command_result out;
  if (child_stdin_ < 0 || shutting_down_)
    return out;

  std::string token = std::to_string(++token_seq_);
  {
    std::lock_guard<std::mutex> lk(reply_mtx_);
    reply_ready_ = false;
    pending_record_ = mi_record{};
    stream_accum_.clear();
  }
  command_in_flight_ = true;

  if (!write_all(child_stdin_, token + command + "\n")) {
    command_in_flight_ = false;
    return out;
  }

  std::unique_lock<std::mutex> lk(reply_mtx_);
  bool got = reply_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [this] { return reply_ready_ || reader_gone_; });
  command_in_flight_ = false;
  if (got && reply_ready_) {
    out.record = pending_record_;
    out.streams = stream_accum_;
    out.ok = out.record.type == mi_record::kind::result && !out.record.is_error();
  }
  return out;
}

// ── Reader thread ─────────────────────────────────────────────────────────

void posix_debug_backend::reader_loop() {
  std::string buf;
  char chunk[4096];

  while (true) {
    ssize_t n = ::read(child_stdout_, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0)
      break; // EOF: debugger exited.
    buf.append(chunk, static_cast<size_t>(n));

    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);

      std::optional<mi_record> rec = parse_mi_line(line);
      if (!rec)
        continue;

      switch (rec->type) {
        case mi_record::kind::result: {
          std::lock_guard<std::mutex> lk(reply_mtx_);
          pending_record_ = std::move(*rec);
          reply_ready_ = true;
          reply_cv_.notify_all();
          break;
        }
        case mi_record::kind::exec_async:
          if (rec->klass == "running") {
            running_ = true;
          } else if (rec->klass == "stopped") {
            running_ = false;
            handle_stopped(*rec);
          }
          break;
        case mi_record::kind::console_stream:
        case mi_record::kind::target_stream:
        case mi_record::kind::log_stream: {
          bool captured = false;
          if (command_in_flight_) {
            std::lock_guard<std::mutex> lk(reply_mtx_);
            if (command_in_flight_) {
              stream_accum_ += rec->klass;
              captured = true;
            }
          }
          // Debuggee stdout (`@`) and out-of-band notices always also reach
          // the Output window; a `~` echo consumed by an in-flight command
          // (e.g. evaluate()'s `whatis`) does not, to avoid duplicating it.
          if (!captured || rec->type == mi_record::kind::target_stream) {
            std::string text = rec->klass;
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
              text.pop_back();
            if (!text.empty()) {
              std::lock_guard<std::mutex> lk(queue_mtx_);
              dispatch_queue_.push_back({false, {}, text, "info"});
              queue_cv_.notify_all();
            }
          }
          break;
        }
        case mi_record::kind::notify_async:
          if (rec->klass == "thread-group-exited") {
            attached_ = false;
            std::lock_guard<std::mutex> lk(queue_mtx_);
            dispatch_queue_.push_back({false, {}, "debuggee exited", "info"});
            queue_cv_.notify_all();
          }
          break;
        default:
          break;
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(reply_mtx_);
    reader_gone_ = true;
    reply_cv_.notify_all();
  }
  attached_ = false;
}

void posix_debug_backend::handle_stopped(const mi_record& rec) {
  std::string reason = rec.get("reason");

  // "exited"/"exited-normally"/"exited-signalled": the inferior is gone --
  // not a pause to surface in the Source window, just a log line.
  if (reason.rfind("exited", 0) == 0) {
    attached_ = false;
    std::lock_guard<std::mutex> lk(queue_mtx_);
    dispatch_queue_.push_back({false, {}, "debuggee exited", "info"});
    queue_cv_.notify_all();
    return;
  }

  stop_event evt;
  evt.thread_id = static_cast<uint32_t>(to_int(rec.get("thread-id")));
  if (evt.thread_id)
    last_stop_thread_ = evt.thread_id;

  if (const mi_value* frame = rec.find("frame")) {
    std::string full = frame->get("fullname");
    evt.file = full.empty() ? frame->get("file") : full;
    evt.line = to_int(frame->get("line"));
  }

  if (!reported_attach_) {
    reported_attach_ = true;
    evt.reason = "attach";
  } else if (reason == "breakpoint-hit") {
    evt.reason = "breakpoint";
  } else if (reason == "end-stepping-range" || reason == "function-finished" ||
             reason == "location-reached") {
    evt.reason = "step";
  } else if (reason == "signal-received") {
    if (pause_requested_.exchange(false))
      evt.reason = "pause";
    else
      evt.reason = "exception";
  } else {
    // Empty or unrecognised reason mid-session (some `lldb-mi` stops):
    // treat as a step completion so the UI still refreshes.
    evt.reason = "step";
  }

  std::lock_guard<std::mutex> lk(queue_mtx_);
  dispatch_queue_.push_back({true, evt, {}, {}});
  queue_cv_.notify_all();
}

// ── Dispatch thread ──────────────────────────────────────────────────────

void posix_debug_backend::dispatch_loop() {
  while (true) {
    dispatch_item item;
    {
      std::unique_lock<std::mutex> lk(queue_mtx_);
      queue_cv_.wait(lk, [this] { return !dispatch_queue_.empty() || shutting_down_; });
      if (dispatch_queue_.empty()) {
        if (shutting_down_)
          return;
        continue;
      }
      item = std::move(dispatch_queue_.front());
      dispatch_queue_.pop_front();
    }

    if (item.is_stop) {
      if (auto cb = *on_stop_cb_.rlock())
        cb(item.stop);
    } else {
      if (auto cb = *on_log_cb_.rlock())
        cb(item.log_text, item.log_level);
    }
  }
}

} // namespace bdg::wish::dbg

#endif // !defined(_WIN32)
