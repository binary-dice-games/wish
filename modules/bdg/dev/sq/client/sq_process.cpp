// MIT License © 2026 Binary Dice Games
/// @file sq_process.cpp
/// @brief libuv-based implementation of run_sq_cli().
///
/// Structurally identical to
/// `modules/bdg/dev/docker/client/docker_process.cpp`, plus optional stdin.
#include "sq_process.hpp"

#include <uv.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

namespace bdg::wish::sq {

namespace {

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

process_result run_sq_cli(
    const std::vector<std::string>& args, const std::string& binary, const std::string& stdin_text) {
  process_result result;
  if (binary.empty())
    return result;

  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0)
    return result;

  // argv[0] is conventionally the program name itself (execve() convention);
  // uv_spawn() PATH-searches a bare name (no path separator) the same way
  // execvp()/CreateProcess() would.
  std::vector<std::string> owned_args;
  owned_args.reserve(args.size() + 1);
  owned_args.push_back(binary);
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

  uv_process_options_t options{};
  options.exit_cb = exit_cb;
  options.file = binary.c_str();
  options.args = argv.data();
  options.stdio_count = 3;
  options.stdio = stdio;

  const int spawn_rc = uv_spawn(&loop, child_req, &options);
  if (spawn_rc != 0) {
    result.stderr_text = uv_strerror(spawn_rc);
    uv_close(reinterpret_cast<uv_handle_t*>(out_pipe), close_cb);
    uv_close(reinterpret_cast<uv_handle_t*>(err_pipe), close_cb);
    if (in_pipe)
      uv_close(reinterpret_cast<uv_handle_t*>(in_pipe), close_cb);
    delete child_req;
    uv_run(&loop, UV_RUN_DEFAULT); // drain the two close callbacks above.
    uv_loop_close(&loop);
    return result;
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

} // namespace bdg::wish::sq
