// MIT License © 2025 Binary Dice Games
/// @file git_process.cpp
/// @brief libuv-based implementation of run_git().
#include "git_process.hpp"

#include <uv.h>

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
  if (buf->base)
    std::free(buf->base);
  if (nread < 0 && !state->closed) {
    state->closed = true;
    uv_close(reinterpret_cast<uv_handle_t*>(stream), close_cb);
  }
}

struct exit_state {
  int64_t exit_status{-1};
};

void exit_cb(uv_process_t* req, int64_t exit_status, int /*term_signal*/) {
  static_cast<exit_state*>(req->data)->exit_status = exit_status;
  uv_close(reinterpret_cast<uv_handle_t*>(req), close_cb);
}

} // namespace

process_result run_git(const std::string& cwd, const std::vector<std::string>& args) {
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
  pipe_state out_state{&result.stdout_text, false};
  pipe_state err_state{&result.stderr_text, false};
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
