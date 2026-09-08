// MIT License © 2026 Binary Dice Games
/// @file python_debug_backend.cpp
/// @brief Implementation of python_debug_backend — a `debugpy` DAP client.
///        See the header for the two attach modes and the threading model.
#include "python_debug_backend.hpp"

#include <uv.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace bdg::wish::dbg {

using dap_json = nlohmann::json;

namespace {

constexpr int kConnectAttempts = 40;   ///< ~6s of 150ms retries.
constexpr int kConnectRetryMs = 150;

/// @brief Splits "host:port" / "port" into its parts. Empty host -> loopback.
std::pair<std::string, int> parse_host_port(const std::string& s) {
  auto colon = s.rfind(':');
  if (colon == std::string::npos)
    return {"127.0.0.1", std::atoi(s.c_str())};
  std::string host = s.substr(0, colon);
  if (host.empty())
    host = "127.0.0.1";
  return {host, std::atoi(s.c_str() + colon + 1)};
}

// ── one-shot blocking child run (debugpy availability probe) ──────────────

struct capture_ctx {
  std::string* out{nullptr};
};

void capture_alloc(uv_handle_t*, size_t n, uv_buf_t* buf) {
  buf->base = static_cast<char*>(std::malloc(n));
  buf->len = buf->base ? n : 0;
}

void capture_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* ctx = static_cast<capture_ctx*>(stream->data);
  if (nread > 0 && ctx->out)
    ctx->out->append(buf->base, static_cast<size_t>(nread));
  if (buf->base)
    std::free(buf->base);
  if (nread < 0)
    uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
}

/// @brief Runs @p argv to completion, capturing its stderr. `argv[0]` is
///        PATH-searched by `uv_spawn` (same as `execvp`).
/// @return The child's exit code, or -1 if it could not be spawned.
int run_blocking(const std::vector<std::string>& argv, std::string* stderr_out) {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0)
    return -1;

  std::vector<char*> cargv;
  cargv.reserve(argv.size() + 1);
  for (const auto& a : argv)
    cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  uv_pipe_t err_pipe;
  uv_pipe_init(&loop, &err_pipe, 0);
  capture_ctx cctx{stderr_out};
  err_pipe.data = &cctx;

  uv_stdio_container_t stdio[3];
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = UV_IGNORE;
  stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&err_pipe);

  uv_process_t proc;
  int64_t exit_code = -1;
  proc.data = &exit_code;

  uv_process_options_t opt{};
  opt.file = argv[0].c_str();
  opt.args = cargv.data();
  opt.stdio = stdio;
  opt.stdio_count = 3;
  opt.exit_cb = [](uv_process_t* r, int64_t status, int) {
    *static_cast<int64_t*>(r->data) = status;
    uv_close(reinterpret_cast<uv_handle_t*>(r), nullptr);
  };

  int rc = uv_spawn(&loop, &proc, &opt);
  if (rc != 0) {
    if (stderr_out)
      *stderr_out = uv_strerror(rc);
    uv_close(reinterpret_cast<uv_handle_t*>(&err_pipe), nullptr);
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    return -1;
  }

  uv_read_start(reinterpret_cast<uv_stream_t*>(&err_pipe), capture_alloc, capture_read);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  return static_cast<int>(exit_code);
}

/// @brief Optional wire trace, `$WISH_DBG_DAP_TRACE` — one line per DAP
///        message in each direction. A debugging aid only.
bool dap_trace_enabled() {
  static const bool on = [] {
    const char* e = std::getenv("WISH_DBG_DAP_TRACE");
    return e && *e;
  }();
  return on;
}

void dap_trace(const char* dir, const std::string& text) {
  if (dap_trace_enabled())
    std::fprintf(stderr, "[dap %s] %s\n", dir, text.c_str());
}

/// @brief Maps a DAP `output` event category to `append_output`'s
///        info/warn/error vocabulary.
std::string level_for_category(const std::string& category) {
  if (category == "stderr" || category == "important")
    return "warn";
  return "info";
}

/// @brief Maps a DAP `stopped` reason to the `stop_event` vocabulary
///        (DESIGN.md §3). `pause_flag` is cleared when a "pause" is claimed.
std::string map_stop_reason(const std::string& dap_reason, std::atomic<bool>& pause_flag) {
  if (dap_reason.find("breakpoint") != std::string::npos)
    return "breakpoint";
  if (dap_reason == "step")
    return "step";
  if (dap_reason == "pause") {
    pause_flag.store(false);
    return "pause";
  }
  if (dap_reason == "exception")
    return "exception";
  if (dap_reason == "entry" || dap_reason == "goto")
    return "step";
  // "signal", "data breakpoint", unknown mid-session: refresh as a step.
  return pause_flag.exchange(false) ? "pause" : "step";
}

} // namespace

// ── libuv handle bundle ──────────────────────────────────────────────────

struct python_debug_backend::uv_state {
  uv_loop_t loop{};
  uv_tcp_t tcp{};
  uv_async_t async{};
  uv_timer_t timer{};
  uv_process_t proc{};
  uv_connect_t connect_req{};

  bool tcp_init{false};
  bool async_init{false};
  bool timer_init{false};
  bool proc_init{false};
  bool connected{false};
  int attempts{0};
};

namespace {

struct write_ctx {
  uv_write_t req{};
  std::string data;
};

} // namespace

// Every libuv handle in `uv_state` stores the owning backend in `.data`.
python_debug_backend* python_debug_backend::backend_of(void* handle_data) {
  return static_cast<python_debug_backend*>(handle_data);
}

// ── Construction / lifecycle ─────────────────────────────────────────────

python_debug_backend::python_debug_backend(std::string connect_endpoint) {
  if (connect_endpoint.empty()) {
    if (const char* env = std::getenv("WISH_DBG_DAP_CONNECT"); env && *env)
      connect_endpoint = env;
  }
  if (!connect_endpoint.empty()) {
    auto [host, port] = parse_host_port(connect_endpoint);
    connect_host_ = host;
    connect_port_ = port;
    use_adapter_ = false;
  }
}

python_debug_backend::~python_debug_backend() {
  teardown();
}

std::string python_debug_backend::discover_python() {
  if (const char* env = std::getenv("WISH_DBG_PYTHON"); env && *env)
    return env;
  return {}; // caller falls back to the candidate list in verify_debugpy().
}

bool python_debug_backend::verify_debugpy(std::string& err) const {
  std::vector<std::string> candidates;
  if (std::string forced = discover_python(); !forced.empty())
    candidates.push_back(forced);
  candidates.push_back("python3");
  candidates.push_back("python");

  for (const auto& py : candidates) {
    std::string ignored;
    if (run_blocking({py, "-c", "import debugpy"}, &ignored) == 0) {
      const_cast<python_debug_backend*>(this)->python_ = py;
      return true;
    }
  }
  err = "debugpy is not importable (tried " +
        [&] {
          std::string s;
          for (size_t i = 0; i < candidates.size(); ++i)
            s += (i ? ", " : "") + candidates[i];
          return s;
        }() +
        "). Install it with `pip install debugpy`, or set $WISH_DBG_PYTHON.";
  return false;
}

int python_debug_backend::pick_free_port() {
  uv_loop_t loop;
  if (uv_loop_init(&loop) != 0)
    return 0;
  uv_tcp_t sock;
  uv_tcp_init(&loop, &sock);
  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 0, &addr);
  int port = 0;
  if (uv_tcp_bind(&sock, reinterpret_cast<const struct sockaddr*>(&addr), 0) == 0) {
    struct sockaddr_storage name{};
    int len = sizeof(name);
    if (uv_tcp_getsockname(&sock, reinterpret_cast<struct sockaddr*>(&name), &len) == 0) {
      auto* sin = reinterpret_cast<struct sockaddr_in*>(&name);
      const auto* p = reinterpret_cast<const unsigned char*>(&sin->sin_port);
      port = (p[0] << 8) | p[1]; // sin_port is network byte order.
    }
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&sock), nullptr);
  uv_run(&loop, UV_RUN_DEFAULT);
  uv_loop_close(&loop);
  return port;
}

// ── debug_backend: attach / detach ───────────────────────────────────────

bool python_debug_backend::attach(uint32_t pid) {
  if (attached_)
    return false;

  auto log_now = [this](const std::string& text, const std::string& level) {
    if (auto cb = *on_log_cb_.rlock())
      cb(text, level);
  };

  if (use_adapter_) {
    std::string err;
    if (!verify_debugpy(err)) {
      log_now(err, "error");
      return false;
    }
    adapter_port_ = pick_free_port();
    if (adapter_port_ <= 0) {
      log_now("could not allocate a local TCP port for debugpy.adapter", "error");
      return false;
    }
    connect_host_ = "127.0.0.1";
    connect_port_ = adapter_port_;
  }

  shutting_down_ = false;
  conn_gone_ = false;
  running_ = true;
  initialized_event_ = false;
  attach_replied_ = false;
  attach_ok_ = false;
  attach_seq_ = 0;
  connect_done_ = false;
  connect_ok_ = false;
  primary_thread_ = 0;
  reader_ = dap_message_reader{};
  *breakpoints_.wlock() = {};
  *frame_ids_.wlock() = {};

  uv_ = std::make_unique<uv_state>();

  dispatch_thread_ = std::thread([this] { dispatch_thread_main(); });
  loop_thread_ = std::thread([this] { loop_thread_main(); });

  {
    std::unique_lock<std::mutex> lk(connect_mtx_);
    connect_cv_.wait_for(lk, std::chrono::seconds(20), [this] { return connect_done_; });
  }
  if (!connect_ok_) {
    log_now(use_adapter_ ? "could not connect to debugpy.adapter" : "could not connect to " + connect_host_ +
                                                                        ":" + std::to_string(connect_port_),
            "error");
    teardown();
    return false;
  }

  if (!do_handshake(pid)) {
    teardown();
    return false;
  }

  attached_ = true;
  if (auto cb = *on_log_cb_.rlock())
    cb(use_adapter_ ? "debugpy: attaching to pid " + std::to_string(pid)
                    : "debugpy: connected to " + connect_host_ + ":" + std::to_string(connect_port_),
       "info");
  return true;
}

void python_debug_backend::detach() {
  if (!loop_thread_.joinable() && !dispatch_thread_.joinable()) {
    attached_ = false;
    return;
  }
  if (attached_) {
    dap_json args;
    args["terminateDebuggee"] = false;
    request("disconnect", args, 3000);
  }
  teardown();
  attached_ = false;
}

void python_debug_backend::teardown() {
  shutting_down_ = true;

  // Wake the loop so on_loop_wakeup() closes the handles and uv_run()
  // returns. Wait for the loop thread to have armed its async handle first
  // (it always does, on every path, within a few uv_*_init calls).
  if (uv_ && loop_thread_.joinable()) {
    while (!loop_ready_)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uv_async_send(&uv_->async);
  }
  if (loop_thread_.joinable())
    loop_thread_.join();

  {
    std::lock_guard<std::mutex> lk(queue_mtx_);
  }
  queue_cv_.notify_all();
  reply_cv_.notify_all();
  init_cv_.notify_all();
  attach_cv_.notify_all();
  if (dispatch_thread_.joinable())
    dispatch_thread_.join();

  uv_.reset();
  attached_ = false;
  running_ = false;
}

// ── libuv loop thread ────────────────────────────────────────────────────

namespace {

void on_alloc(uv_handle_t*, size_t n, uv_buf_t* buf) {
  buf->base = static_cast<char*>(std::malloc(n));
  buf->len = buf->base ? n : 0;
}

} // namespace

void python_debug_backend::loop_thread_main() {
  uv_loop_init(&uv_->loop);

  // Init async/timer/tcp *first* and unconditionally, so teardown() can
  // always stop the loop via uv_async_send() even if the adapter spawn
  // below fails.
  uv_->async.data = this;
  uv_async_init(&uv_->loop, &uv_->async, [](uv_async_t* a) { backend_of(a->data)->on_loop_wakeup(); });
  uv_->async_init = true;

  uv_->timer.data = this;
  uv_timer_init(&uv_->loop, &uv_->timer);
  uv_->timer_init = true;

  uv_->tcp.data = this;
  uv_tcp_init(&uv_->loop, &uv_->tcp);
  uv_->tcp_init = true;

  // The async handle is now armed; teardown() may signal it from here on
  // (uv_async_send latches even before uv_run() is entered).
  loop_ready_ = true;

  bool ok_to_connect = true;
  if (use_adapter_) {
    std::vector<std::string> argv = {python_,       "-m",   "debugpy.adapter", "--host", "127.0.0.1",
                                     "--port",      std::to_string(adapter_port_)};
    std::vector<char*> cargv;
    for (auto& a : argv)
      cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_IGNORE;
    stdio[2].flags = UV_IGNORE;

    uv_process_options_t opt{};
    opt.file = python_.c_str();
    opt.args = cargv.data();
    opt.stdio = stdio;
    opt.stdio_count = 3;
    opt.exit_cb = [](uv_process_t* p, int64_t, int) {
      auto* self = backend_of(p->data);
      if (!self->uv_->connected) {
        std::lock_guard<std::mutex> lk(self->connect_mtx_);
        self->connect_done_ = true;
        self->connect_ok_ = false;
        self->connect_cv_.notify_all();
      }
    };
    uv_->proc.data = this;
    if (uv_spawn(&uv_->loop, &uv_->proc, &opt) == 0) {
      uv_->proc_init = true;
    } else {
      ok_to_connect = false;
      std::lock_guard<std::mutex> lk(connect_mtx_);
      connect_done_ = true;
      connect_ok_ = false;
      connect_cv_.notify_all();
    }
  }

  if (ok_to_connect)
    uv_timer_start(
        &uv_->timer, [](uv_timer_t* t) { backend_of(t->data)->begin_connect(); },
        use_adapter_ ? 300 : 0, 0);

  uv_run(&uv_->loop, UV_RUN_DEFAULT);
  loop_ready_ = false;
  uv_run(&uv_->loop, UV_RUN_DEFAULT); // drain any pending close callbacks.
  uv_loop_close(&uv_->loop);
}

void python_debug_backend::begin_connect() {
  if (shutting_down_)
    return;
  uv_->attempts++;
  struct sockaddr_in addr;
  uv_ip4_addr(connect_host_.c_str(), connect_port_, &addr);
  int rc = uv_tcp_connect(
      &uv_->connect_req, &uv_->tcp, reinterpret_cast<const struct sockaddr*>(&addr),
      [](uv_connect_t* req, int status) { backend_of(req->handle->data)->on_connect_result(status); });
  if (rc != 0)
    on_connect_result(rc);
}

void python_debug_backend::on_connect_result(int status) {
  if (status == 0) {
    uv_->connected = true;
    uv_read_start(reinterpret_cast<uv_stream_t*>(&uv_->tcp), on_alloc, [](uv_stream_t* s, ssize_t nread, const uv_buf_t* buf) {
      auto* self = backend_of(s->data);
      if (nread > 0)
        self->on_socket_bytes(buf->base, static_cast<size_t>(nread));
      if (buf->base)
        std::free(buf->base);
      if (nread < 0) {
        self->uv_->connected = false;
        {
          std::lock_guard<std::mutex> lk(self->reply_mtx_);
          self->conn_gone_ = true;
        }
        self->reply_cv_.notify_all();
        self->init_cv_.notify_all();
        self->attach_cv_.notify_all();
        if (self->attached_)
          self->queue_log("debugpy connection closed", "warn");
        self->attached_ = false;
        uv_read_stop(s);
      }
    });
    std::lock_guard<std::mutex> lk(connect_mtx_);
    connect_done_ = true;
    connect_ok_ = true;
    connect_cv_.notify_all();
    return;
  }

  // Failed — close the tcp handle, re-init it, and retry (the adapter /
  // server socket may simply not be listening yet).
  if (uv_->attempts >= kConnectAttempts || shutting_down_) {
    {
      std::lock_guard<std::mutex> lk(connect_mtx_);
      connect_done_ = true;
      connect_ok_ = false;
      connect_cv_.notify_all();
    }
    return;
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&uv_->tcp), [](uv_handle_t* h) {
    auto* self = backend_of(h->data);
    if (self->shutting_down_)
      return;
    uv_tcp_init(&self->uv_->loop, &self->uv_->tcp);
    self->uv_->tcp.data = self;
    uv_timer_start(
        &self->uv_->timer,
        [](uv_timer_t* t) { backend_of(t->data)->begin_connect(); },
        kConnectRetryMs, 0);
  });
}

void python_debug_backend::on_loop_wakeup() {
  if (shutting_down_) {
    auto close_h = [](uv_handle_t* h) {
      if (h && !uv_is_closing(h))
        uv_close(h, nullptr);
    };
    if (uv_->proc_init)
      uv_process_kill(&uv_->proc, SIGTERM);
    if (uv_->tcp_init)
      close_h(reinterpret_cast<uv_handle_t*>(&uv_->tcp));
    if (uv_->timer_init)
      close_h(reinterpret_cast<uv_handle_t*>(&uv_->timer));
    if (uv_->async_init)
      close_h(reinterpret_cast<uv_handle_t*>(&uv_->async));
    if (uv_->proc_init)
      close_h(reinterpret_cast<uv_handle_t*>(&uv_->proc));
    return;
  }

  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lk(write_mtx_);
    pending.swap(write_queue_);
  }
  for (auto& frame : pending) {
    if (!uv_->connected)
      break;
    auto* ctx = new write_ctx();
    ctx->data = std::move(frame);
    ctx->req.data = ctx;
    uv_buf_t buf = uv_buf_init(ctx->data.data(), static_cast<unsigned>(ctx->data.size()));
    uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(&uv_->tcp), &buf, 1, [](uv_write_t* req, int) {
      delete static_cast<write_ctx*>(req->data);
    });
  }
}

void python_debug_backend::enqueue_write(std::string framed) {
  if (dap_trace_enabled())
    dap_trace("->", framed.substr(framed.find("\r\n\r\n") + 4));
  {
    std::lock_guard<std::mutex> lk(write_mtx_);
    write_queue_.push_back(std::move(framed));
  }
  if (uv_)
    uv_async_send(&uv_->async);
}

// ── inbound DAP ──────────────────────────────────────────────────────────

void python_debug_backend::on_socket_bytes(const char* data, size_t n) {
  reader_.feed(std::string_view(data, n));
  while (auto msg = reader_.next()) {
    if (dap_trace_enabled())
      dap_trace("<-", msg->dump());
    handle_dap_message(*msg);
  }
}

void python_debug_backend::handle_dap_message(const dap_json& msg) {
  const std::string type = msg.value("type", std::string());

  if (type == "response") {
    const int rseq = msg.value("request_seq", -1);
    if (attach_seq_ != 0 && rseq == attach_seq_) {
      std::lock_guard<std::mutex> lk(attach_mtx_);
      attach_replied_ = true;
      attach_ok_ = msg.value("success", false);
      attach_msg_ = msg.value("message", std::string());
      attach_cv_.notify_all();
      return;
    }
    std::lock_guard<std::mutex> lk(reply_mtx_);
    if (rseq == pending_seq_) {
      pending_response_ = msg;
      reply_ready_ = true;
      reply_cv_.notify_all();
    }
    return;
  }

  if (type != "event")
    return;

  const std::string ev = msg.value("event", std::string());
  const dap_json body = msg.contains("body") && msg["body"].is_object() ? msg["body"] : dap_json::object();

  if (ev == "initialized") {
    std::lock_guard<std::mutex> lk(init_mtx_);
    initialized_event_ = true;
    init_cv_.notify_all();
  } else if (ev == "stopped") {
    running_ = false;
    uint32_t tid = body.value("threadId", 0u);
    if (tid)
      primary_thread_ = tid;
    queue_stop(tid, body.value("reason", std::string()));
  } else if (ev == "continued") {
    running_ = true;
  } else if (ev == "output") {
    std::string category = body.value("category", std::string());
    if (category == "telemetry")
      return;
    std::string text = body.value("output", std::string());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();
    if (!text.empty())
      queue_log(text, level_for_category(category));
  } else if (ev == "process") {
    std::string name = body.value("name", std::string());
    long long spid = body.value("systemProcessId", 0LL);
    queue_log("attached to " + (name.empty() ? "process" : name) +
                  (spid ? " (pid " + std::to_string(spid) + ")" : ""),
              "info");
  } else if (ev == "exited") {
    attached_ = false;
    queue_log("debuggee exited (code " + std::to_string(body.value("exitCode", 0)) + ")", "info");
  } else if (ev == "terminated") {
    attached_ = false;
    queue_log("debug session terminated", "info");
  }
}

// ── request / response ───────────────────────────────────────────────────

python_debug_backend::dap_response python_debug_backend::request(const std::string& command,
                                                                 dap_json arguments, int timeout_ms) {
  std::lock_guard<std::mutex> send_lk(send_mtx_);
  return request_locked(command, std::move(arguments), timeout_ms);
}

python_debug_backend::dap_response python_debug_backend::request_locked(const std::string& command,
                                                                       dap_json arguments,
                                                                       int timeout_ms) {
  dap_response out;
  if (conn_gone_ || shutting_down_ || !uv_)
    return out;

  const int seq = next_seq();
  {
    std::lock_guard<std::mutex> lk(reply_mtx_);
    pending_seq_ = seq;
    reply_ready_ = false;
    pending_response_ = dap_json::object();
  }
  enqueue_write(dap_frame(dap_make_request(seq, command, std::move(arguments))));

  std::unique_lock<std::mutex> lk(reply_mtx_);
  reply_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [this] { return reply_ready_ || conn_gone_; });
  if (reply_ready_) {
    out.body = pending_response_.contains("body") && pending_response_["body"].is_object()
                   ? pending_response_["body"]
                   : dap_json::object();
    out.ok = pending_response_.value("success", false);
    if (!out.ok)
      out.message = pending_response_.value("message", std::string());
  }
  return out;
}

bool python_debug_backend::do_handshake(uint32_t pid) {
  std::lock_guard<std::mutex> send_lk(send_mtx_);

  dap_json init_args = {
      {"clientID", "wish"},          {"clientName", "wish dbg"}, {"adapterID", "debugpy"},
      {"pathFormat", "path"},        {"linesStartAt1", true},    {"columnsStartAt1", true},
      {"supportsVariableType", true},
  };
  dap_response init = request_locked("initialize", std::move(init_args), 10000);
  if (!init.ok) {
    if (auto cb = *on_log_cb_.rlock())
      cb("debugpy initialize failed" + (init.message.empty() ? "" : ": " + init.message), "error");
    return false;
  }

  // adapter mode: the adapter injects `debugpy` into the target by PID.
  // connect mode: we are already connected straight to the target's own
  // `debugpy.listen()` server, so `attach` carries no locator at all.
  dap_json attach_args;
  attach_args["justMyCode"] = false;
  if (use_adapter_)
    attach_args["processId"] = pid;

  // 'attach' does not complete until after 'configurationDone' (its
  // response is delayed behind the 'initialized' -> setBreakpoints ->
  // configurationDone exchange), so fire it without blocking and track its
  // seq separately (handle_dap_message routes the late response to
  // attach_cv_).
  attach_seq_ = next_seq();
  {
    std::lock_guard<std::mutex> lk(attach_mtx_);
    attach_replied_ = false;
  }
  enqueue_write(dap_frame(dap_make_request(attach_seq_, "attach", std::move(attach_args))));

  {
    std::unique_lock<std::mutex> lk(init_mtx_);
    init_cv_.wait_for(lk, std::chrono::seconds(15), [this] { return initialized_event_ || conn_gone_; });
  }

  request_locked("configurationDone", dap_json::object(), 5000);

  {
    std::unique_lock<std::mutex> lk(attach_mtx_);
    attach_cv_.wait_for(lk, std::chrono::seconds(10), [this] { return attach_replied_ || conn_gone_; });
    if (attach_replied_ && !attach_ok_) {
      if (auto cb = *on_log_cb_.rlock())
        cb("debugpy attach failed" + (attach_msg_.empty() ? "" : ": " + attach_msg_), "error");
      return false;
    }
    if (!attach_replied_ && conn_gone_)
      return false;
  }
  return true;
}

// ── dispatch thread ──────────────────────────────────────────────────────

void python_debug_backend::queue_stop(uint32_t thread_id, std::string dap_reason) {
  {
    std::lock_guard<std::mutex> lk(queue_mtx_);
    dispatch_item item;
    item.is_stop = true;
    item.thread_id = thread_id;
    item.reason = std::move(dap_reason);
    dispatch_queue_.push_back(std::move(item));
  }
  queue_cv_.notify_all();
}

void python_debug_backend::queue_log(std::string text, std::string level) {
  {
    std::lock_guard<std::mutex> lk(queue_mtx_);
    dispatch_item item;
    item.log_text = std::move(text);
    item.log_level = std::move(level);
    dispatch_queue_.push_back(std::move(item));
  }
  queue_cv_.notify_all();
}

void python_debug_backend::dispatch_thread_main() {
  for (;;) {
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

    if (!item.is_stop) {
      if (auto cb = *on_log_cb_.rlock())
        cb(item.log_text, item.log_level);
      continue;
    }

    stop_event ev;
    ev.thread_id = item.thread_id;
    ev.reason = map_stop_reason(item.reason, pause_requested_);

    dap_json args = {{"threadId", item.thread_id}, {"startFrame", 0}, {"levels", 1}};
    dap_response r = request("stackTrace", std::move(args), 5000);
    if (r.ok && r.body.contains("stackFrames") && r.body["stackFrames"].is_array() &&
        !r.body["stackFrames"].empty()) {
      const dap_json& f0 = r.body["stackFrames"][0];
      if (f0.contains("source") && f0["source"].is_object())
        ev.file = f0["source"].value("path", std::string());
      ev.line = f0.value("line", 0);
    }

    if (auto cb = *on_stop_cb_.rlock())
      cb(ev);
  }
}

// ── debug_backend: execution control ─────────────────────────────────────

void python_debug_backend::pause() {
  if (!attached_)
    return;
  pause_requested_ = true;
  uint32_t tid = primary_thread_.load();
  dap_json args;
  args["threadId"] = tid ? tid : 1u;
  request("pause", std::move(args), 3000);
}

void python_debug_backend::resume() {
  // The Source toolbar only enables Continue while paused; guard anyway,
  // since `continue` issued to an already-running debuggee leaves debugpy
  // waiting and never delivers the next `stopped`.
  if (!attached_ || running_.load())
    return;
  pause_requested_ = false;
  running_ = true;
  uint32_t tid = primary_thread_.load();
  dap_json args;
  args["threadId"] = tid ? tid : 1u;
  request("continue", std::move(args));
}

void python_debug_backend::step_into(uint32_t thread_id) {
  if (!attached_)
    return;
  dap_json args;
  args["threadId"] = thread_id ? thread_id : primary_thread_.load();
  request("stepIn", std::move(args));
}

void python_debug_backend::step_over(uint32_t thread_id) {
  if (!attached_)
    return;
  dap_json args;
  args["threadId"] = thread_id ? thread_id : primary_thread_.load();
  request("next", std::move(args));
}

void python_debug_backend::step_out(uint32_t thread_id) {
  if (!attached_)
    return;
  dap_json args;
  args["threadId"] = thread_id ? thread_id : primary_thread_.load();
  request("stepOut", std::move(args));
}

// ── debug_backend: breakpoints ───────────────────────────────────────────

bool python_debug_backend::set_breakpoint(const std::string& file, int line) {
  if (!attached_)
    return false;
  (*breakpoints_.wlock())[file].insert(line);
  return resend_breakpoints(file, line);
}

void python_debug_backend::clear_breakpoint(const std::string& file, int line) {
  {
    auto wl = breakpoints_.wlock();
    auto it = wl->find(file);
    if (it == wl->end())
      return;
    it->second.erase(line);
    if (it->second.empty())
      wl->erase(it);
  }
  if (attached_)
    resend_breakpoints(file, 0);
}

bool python_debug_backend::resend_breakpoints(const std::string& file, int want_line) {
  std::set<int> lines;
  {
    auto rl = breakpoints_.rlock();
    if (auto it = rl->find(file); it != rl->end())
      lines = it->second;
  }
  dap_json bps = dap_json::array();
  for (int l : lines)
    bps.push_back({{"line", l}});

  dap_json args;
  args["source"] = {{"path", file}};
  args["breakpoints"] = std::move(bps);
  dap_response r = request("setBreakpoints", std::move(args));

  if (want_line == 0)
    return true;
  if (!r.ok || !r.body.contains("breakpoints") || !r.body["breakpoints"].is_array())
    return false;
  for (const dap_json& b : r.body["breakpoints"])
    if (b.value("line", -1) == want_line)
      return b.value("verified", false);
  return false;
}

// ── debug_backend: inspection ────────────────────────────────────────────

std::vector<thread_info> python_debug_backend::get_threads() {
  std::vector<thread_info> out;
  if (!attached_)
    return out;
  dap_response r = request("threads");
  if (!r.ok || !r.body.contains("threads") || !r.body["threads"].is_array())
    return out;

  const bool stopped = !running_.load();
  for (const dap_json& t : r.body["threads"]) {
    thread_info ti;
    ti.id = t.value("id", 0u);
    ti.state = stopped ? "suspended" : "running";
    if (stopped) {
      dap_json args = {{"threadId", ti.id}, {"startFrame", 0}, {"levels", 1}};
      dap_response sr = request("stackTrace", std::move(args), 4000);
      if (sr.ok && sr.body.contains("stackFrames") && sr.body["stackFrames"].is_array() &&
          !sr.body["stackFrames"].empty())
        ti.current_function = sr.body["stackFrames"][0].value("name", std::string());
    }
    out.push_back(std::move(ti));
  }
  return out;
}

std::vector<frame_info> python_debug_backend::get_callstack(uint32_t thread_id) {
  std::vector<frame_info> out;
  if (!attached_)
    return out;
  dap_json args;
  args["threadId"] = thread_id;
  dap_response r = request("stackTrace", std::move(args));
  if (!r.ok || !r.body.contains("stackFrames") || !r.body["stackFrames"].is_array())
    return out;

  auto fl = frame_ids_.wlock();
  fl->clear();
  uint32_t idx = 0;
  for (const dap_json& f : r.body["stackFrames"]) {
    frame_info fi;
    fi.index = static_cast<int32_t>(idx);
    fi.function = f.value("name", std::string());
    if (f.contains("source") && f["source"].is_object())
      fi.file = f["source"].value("path", std::string());
    fi.line = f.value("line", 0);
    (*fl)[idx] = f.value("id", static_cast<int64_t>(0));
    out.push_back(std::move(fi));
    ++idx;
  }
  return out;
}

std::vector<watch_entry> python_debug_backend::evaluate(uint32_t frame_id,
                                                       const std::vector<std::string>& exprs) {
  std::vector<watch_entry> out;
  if (!attached_ || exprs.empty())
    return out;

  int64_t dap_frame = 0;
  {
    auto rl = frame_ids_.rlock();
    if (auto it = rl->find(frame_id); it != rl->end())
      dap_frame = it->second;
  }

  for (const auto& expr : exprs) {
    dap_json args = {{"expression", expr}, {"context", "watch"}};
    if (dap_frame != 0)
      args["frameId"] = dap_frame;
    dap_response r = request("evaluate", std::move(args));
    if (!r.ok)
      continue;
    watch_entry w;
    w.name = expr;
    w.value = r.body.value("result", std::string());
    w.type = r.body.value("type", std::string());
    out.push_back(std::move(w));
  }
  return out;
}

// ── debug_backend: callbacks ─────────────────────────────────────────────

void python_debug_backend::on_stop(stop_callback cb) {
  *on_stop_cb_.wlock() = std::move(cb);
}

void python_debug_backend::on_log(log_callback cb) {
  *on_log_cb_.wlock() = std::move(cb);
}

} // namespace bdg::wish::dbg
