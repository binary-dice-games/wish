// MIT License © 2026 Binary Dice Games
/// @file process.cpp
/// @brief libuv-based implementation of run_process().
#include "modules/bdg/dev/common/process.hpp"

#include <uv.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>

namespace bdg::wish::dev {

// ── libuv plumbing ───────────────────────────────────────────────────────────

namespace {

struct pipe_state {
  std::string* out{nullptr};
  bool closed{false};
  uv_pipe_t* handle{nullptr}; // valid while !closed
  const common::run_hooks* hooks{nullptr};
};

void alloc_cb(uv_handle_t*, size_t suggested_size, uv_buf_t* buf) {
  buf->base = static_cast<char*>(std::malloc(suggested_size));
  buf->len = buf->base ? suggested_size : 0;
}

void close_cb(uv_handle_t* handle) {
  delete handle;
}

void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* state = static_cast<pipe_state*>(stream->data);
  if (nread > 0 && state->out)
    state->out->append(buf->base, static_cast<size_t>(nread));
  if (nread > 0 && state->hooks && state->hooks->on_output)
    state->hooks->on_output(std::string{buf->base, static_cast<size_t>(nread)});
  if (buf->base)
    std::free(buf->base);
  if (nread < 0 && !state->closed) {
    state->closed = true;
    uv_close(reinterpret_cast<uv_handle_t*>(stream), close_cb);
  }
}

struct exit_state {
  int64_t exit_status{-1};
  uv_timer_t* timer{nullptr};           // the on_tick timer, closed when the child exits
  bool killed{false};                   // on_tick asked for a stop
  pipe_state* pipes[2]{nullptr, nullptr}; // the child's stdout / stderr
};

void exit_cb(uv_process_t* req, int64_t exit_status, int term_signal) {
  auto* state = static_cast<exit_state*>(req->data);
  // A signalled child (on_tick asked for a stop) reports exit status 0.
  state->exit_status = term_signal != 0 && exit_status == 0 ? 128 + term_signal : exit_status;
  if (state->timer) {
    uv_timer_stop(state->timer);
    uv_close(reinterpret_cast<uv_handle_t*>(state->timer), close_cb);
    state->timer = nullptr;
  }
  // A stopped tool may leave a helper process behind (git's remote helper,
  // a pip build) that still holds the output pipes open: stop reading now
  // rather than wait for it, or the run call would not return.
  if (state->killed) {
    for (pipe_state* ps : state->pipes) {
      if (ps && !ps->closed) {
        ps->closed = true;
        uv_close(reinterpret_cast<uv_handle_t*>(ps->handle), close_cb);
      }
    }
  }
  uv_close(reinterpret_cast<uv_handle_t*>(req), close_cb);
}

struct tick_state {
  const common::run_hooks* hooks{nullptr};
  uv_process_t* child{nullptr};
  exit_state* exit{nullptr};
};

void tick_cb(uv_timer_t* timer) {
  auto* state = static_cast<tick_state*>(timer->data);
  if (!state->exit->killed && !state->hooks->on_tick()) {
    state->exit->killed = true; // exit_cb closes the timer once the child is gone.
    uv_process_kill(state->child, SIGTERM);
  }
}

struct write_state {
  uv_write_t req;
  std::string data;
};

// Closing the pipe after the single write delivers EOF to the child.
void write_cb(uv_write_t* req, int /*status*/) {
  auto* w = reinterpret_cast<write_state*>(req);
  uv_close(reinterpret_cast<uv_handle_t*>(req->handle), close_cb);
  delete w;
}

} // namespace

process_result run_process(const std::vector<std::string>& argv, const process_options& options) {
  process_result result;
  if (argv.empty() || argv[0].empty()) {
    result.stderr_text = "no program to run";
    return result;
  }

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0) {
    result.stderr_text = "cannot initialize the event loop";
    return result;
  }

  std::vector<std::string> owned_args = argv;
  std::vector<char*> c_argv;
  c_argv.reserve(owned_args.size() + 1);
  for (auto& a : owned_args)
    c_argv.push_back(a.data());
  c_argv.push_back(nullptr);

  auto* out_pipe = new uv_pipe_t;
  auto* err_pipe = new uv_pipe_t;
  uv_pipe_init(&loop, out_pipe, 0);
  uv_pipe_init(&loop, err_pipe, 0);
  pipe_state out_state{&result.stdout_text, false, nullptr, options.hooks};
  pipe_state err_state{&result.stderr_text, false, nullptr, options.hooks};
  out_pipe->data = &out_state;
  err_pipe->data = &err_state;

  const std::string& stdin_text = options.stdin_text;
  // A child that exits without reading stdin (e.g. `sq add` rejecting a bad
  // location) would otherwise turn our write into a fatal SIGPIPE.
#ifndef _WIN32
  if (!stdin_text.empty())
    std::signal(SIGPIPE, SIG_IGN);
#endif
  uv_pipe_t* in_pipe = nullptr;
  uv_stdio_container_t stdio[3];
  if (stdin_text.empty()) {
    stdio[0].flags = UV_IGNORE;
  } else {
    in_pipe = new uv_pipe_t;
    uv_pipe_init(&loop, in_pipe, 0);
    stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
    stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(in_pipe);
  }
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(out_pipe);
  stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(err_pipe);

  auto* child_req = new uv_process_t;
  exit_state exit_st;
  child_req->data = &exit_st;

  uv_process_options_t uv_options{};
  uv_options.exit_cb = exit_cb;
  uv_options.file = owned_args[0].c_str();
  uv_options.args = c_argv.data();
  uv_options.cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
  uv_options.stdio_count = 3;
  uv_options.stdio = stdio;

  const int spawn_rc = uv_spawn(&loop, child_req, &uv_options);
  if (spawn_rc != 0) {
    result.stderr_text = uv_strerror(spawn_rc);
    uv_close(reinterpret_cast<uv_handle_t*>(out_pipe), close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(err_pipe), close_cb);
    if (in_pipe)
      uv_close(reinterpret_cast<uv_handle_t*>(in_pipe), close_cb);
    // A handle uv_spawn() failed on is still registered with the loop and
    // must be uv_close()d, not just freed: otherwise uv_loop_close() below
    // refuses (EBUSY) and leaves the loop's SIGCHLD watcher in libuv's
    // process-wide signal tree, pointing into this dead stack frame -- the
    // next spawn in the process then crashes walking that tree.
    uv_close(reinterpret_cast<uv_handle_t*>(child_req), close_cb);
    uv_run(&loop, UV_RUN_DEFAULT); // drain the close callbacks above.
    uv_loop_close(&loop);
    return result;
  }

  out_state.handle = out_pipe;
  err_state.handle = err_pipe;
  exit_st.pipes[0] = &out_state;
  exit_st.pipes[1] = &err_state;
  tick_state tick_st{options.hooks, child_req, &exit_st};
  if (options.hooks && options.hooks->on_tick) {
    exit_st.timer = new uv_timer_t;
    uv_timer_init(&loop, exit_st.timer);
    exit_st.timer->data = &tick_st;
    uv_timer_start(exit_st.timer, tick_cb, options.hooks->tick_ms, options.hooks->tick_ms);
  }

  if (in_pipe) {
    auto* w = new write_state;
    w->data = stdin_text;
    uv_buf_t buf = uv_buf_init(w->data.data(), static_cast<unsigned int>(w->data.size()));
    if (uv_write(&w->req, reinterpret_cast<uv_stream_t*>(in_pipe), &buf, 1, write_cb) != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(in_pipe), close_cb);
      delete w;
    }
  }
  uv_read_start(reinterpret_cast<uv_stream_t*>(out_pipe), alloc_cb, read_cb);
  uv_read_start(reinterpret_cast<uv_stream_t*>(err_pipe), alloc_cb, read_cb);

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);

  result.exit_code = static_cast<int>(exit_st.exit_status);
  return result;
}

} // namespace bdg::wish::dev
