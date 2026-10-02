// MIT License © 2025 Binary Dice Games
/// @file git_process.cpp
/// @brief libuv-based implementation of run_git().
#include "git_process.hpp"

#include <uv.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#endif

namespace bdg::wish::git {

namespace {

// Ensures GIT_TERMINAL_PROMPT=0 is set in *this process's* environment
// exactly once -- every uv_spawn()'d child below inherits it (options.env
// is left null, i.e. "inherit"). A tiny, narrowly-scoped platform guard
// (CLAUDE.md's convention for a small divergence) rather than a separate
// _posix/_win file: setenv()/_putenv_s() are the only differing calls.
void ensure_git_terminal_prompt_disabled() {
  static const bool done = [] {
#if defined(_WIN32)
    _putenv_s("GIT_TERMINAL_PROMPT", "0");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
#endif
    return true;
  }();
  (void)done;
}

struct pipe_state {
  std::string* out{nullptr};
  bool closed{false};
  uv_pipe_t* handle{nullptr}; // valid while !closed
  const dev::run_hooks* hooks{nullptr};
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
  uv_timer_t* timer{nullptr}; // the on_tick timer, closed when the child exits
  bool killed{false};         // on_tick asked for a stop
  struct pipe_state* pipes[2]{nullptr, nullptr}; // the child's stdout / stderr
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
  const dev::run_hooks* hooks{nullptr};
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

} // namespace

process_result run_git(const std::string& cwd, const std::vector<std::string>& args, const dev::run_hooks* hooks) {
  ensure_git_terminal_prompt_disabled();

  process_result result;
  if (cwd.empty())
    return result;

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0)
    return result;

  // argv[0] is conventionally the program name itself (execve() convention);
  // uv_spawn() PATH-searches "git" on POSIX/Windows since it contains no
  // path separator, same as execvp()/CreateProcess() with no explicit path.
  std::vector<std::string> owned_args;
  owned_args.reserve(args.size() + 1);
  owned_args.push_back("git");
  for (auto& a : args)
    owned_args.push_back(a);
  std::vector<char*> argv;
  argv.reserve(owned_args.size() + 1);
  for (auto& a : owned_args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  auto* out_pipe = new uv_pipe_t;
  auto* err_pipe = new uv_pipe_t;
  uv_pipe_init(&loop, out_pipe, 0);
  uv_pipe_init(&loop, err_pipe, 0);
  pipe_state out_state{&result.stdout_text, false, nullptr, hooks};
  pipe_state err_state{&result.stderr_text, false, nullptr, hooks};
  out_pipe->data = &out_state;
  err_pipe->data = &err_state;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(out_pipe);
  stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(err_pipe);

  auto* child_req = new uv_process_t;
  exit_state exit_st;
  child_req->data = &exit_st;

  uv_process_options_t options{};
  options.exit_cb = exit_cb;
  options.file = "git";
  options.args = argv.data();
  options.cwd = cwd.c_str();
  options.stdio_count = 3;
  options.stdio = stdio;

  const int spawn_rc = uv_spawn(&loop, child_req, &options);
  if (spawn_rc != 0) {
    result.stderr_text = uv_strerror(spawn_rc);
    uv_close(reinterpret_cast<uv_handle_t*>(out_pipe), close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(err_pipe), close_cb);
    delete child_req;
    uv_run(&loop, UV_RUN_DEFAULT); // drain the two close callbacks above.
    uv_loop_close(&loop);
    return result;
  }

  out_state.handle = out_pipe;
  err_state.handle = err_pipe;
  exit_st.pipes[0] = &out_state;
  exit_st.pipes[1] = &err_state;
  tick_state tick_st{hooks, child_req, &exit_st};
  if (hooks && hooks->on_tick) {
    exit_st.timer = new uv_timer_t;
    uv_timer_init(&loop, exit_st.timer);
    exit_st.timer->data = &tick_st;
    uv_timer_start(exit_st.timer, tick_cb, hooks->tick_ms, hooks->tick_ms);
  }

  uv_read_start(reinterpret_cast<uv_stream_t*>(out_pipe), alloc_cb, read_cb);
  uv_read_start(reinterpret_cast<uv_stream_t*>(err_pipe), alloc_cb, read_cb);

  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);

  result.exit_code = static_cast<int>(exit_st.exit_status);
  return result;
}

std::string resolve_repo_root(const std::string& path) {
  auto r = run_git(path, {"rev-parse", "--show-toplevel"});
  if (!r.ok() || r.stdout_text.empty())
    return path;
  std::string root = r.stdout_text;
  while (!root.empty() && (root.back() == '\n' || root.back() == '\r'))
    root.pop_back();
  return root;
}

} // namespace bdg::wish::git
