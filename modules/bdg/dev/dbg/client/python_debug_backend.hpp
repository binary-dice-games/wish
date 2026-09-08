// MIT License © 2026 Binary Dice Games
/// @file python_debug_backend.hpp
/// @brief `debug_backend` implementation for debugging **Python** processes,
///        driving Microsoft's `debugpy` over the Debug Adapter Protocol
///        (DAP) — the exact adapter and protocol VS Code's Python debugger
///        uses.
///
/// Selected at run time with `wish client --run=dbg -- --backend python`
/// (see `client/dbg.cpp`). Like `posix_debug_backend` drives a child
/// `gdb --interpreter=mi`, this drives `debugpy` and never re-implements a
/// Python tracer of its own — the "GUI frontend over the real tool via its
/// machine-interface protocol" split the whole `dbg` module (and
/// `docker`/`git`/`kubectl`) follow.
///
/// Two attach modes, behind the one `attach(pid)` entry point:
///
/// - **adapter mode** (default): spawn `python -m debugpy.adapter
///   --host 127.0.0.1 --port <auto>` and connect to it over TCP, then issue
///   a DAP `attach` with `{"processId": <pid>}`. `debugpy` injects its
///   debug server into the target — which, exactly like `ptrace` for the
///   GDB backend, needs OS support (`gdb`/`lldb` for the injector, or a
///   CPython new enough for `sys.remote_exec`); on a locked-down host the
///   attach fails with a logged DAP `output` message, same graceful
///   failure as the GDB backend.
/// - **connect mode**: when `$WISH_DBG_DAP_CONNECT` (or the `--connect
///   host:port` module arg) is set, skip the adapter and connect straight
///   to a `debugpy` server the target already started itself
///   (`debugpy.listen((host, port))` in its own code). No injection, works
///   everywhere. This is also the mode the backend's own live test uses.
///
/// Cross-platform: the child process (`uv_spawn`) and the socket
/// (`uv_tcp_t`) both come from libuv, already built into every wish binary
/// (`uv_a`, linked by `cmake/WishModules.cmake`) — so unlike
/// `posix_debug_backend` this file is compiled on Windows too.
#pragma once

#include "dap_protocol.hpp"
#include "debug_backend.hpp"

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish::dbg {

/// @brief `debug_backend` backed by a `debugpy` DAP session.
///
/// Threading model (mirrors `posix_debug_backend`'s reader/dispatch split):
/// - The **loop thread** owns the libuv loop: the TCP socket, the outbound
///   write queue (fed via `uv_async`), and — in adapter mode — the child
///   `debugpy.adapter` process. It parses every inbound DAP message and
///   routes it: `response` records complete the one in-flight `request()`;
///   `event` records (`stopped`, `output`, `continued`, ...) are pushed to
///   `dispatch_queue_`.
/// - The **dispatch thread** drains `dispatch_queue_` and invokes the
///   `on_stop`/`on_log` callbacks. Separate from the loop thread precisely
///   so a callback (`dbg_source::handle_stop`) can re-enter
///   `get_threads()`/`get_callstack()`/`evaluate()` — each issuing its own
///   `request()` — without deadlocking the thread that must service that
///   request's reply.
/// - Public methods run on the RMI dispatch thread and serialise through
///   `request()` (`send_mtx_`, one outstanding request, correlated by DAP
///   `seq`).
class python_debug_backend : public debug_backend {
 public:
  /// @param connect_endpoint  Optional `host:port`. When non-empty (or when
  ///        `$WISH_DBG_DAP_CONNECT` is set), the backend uses *connect mode*
  ///        (see the file doc comment) instead of spawning an adapter.
  explicit python_debug_backend(std::string connect_endpoint = {});
  ~python_debug_backend() override;

  bool attach(uint32_t pid) override;
  void detach() override;
  void pause() override;
  void resume() override;
  void step_into(uint32_t thread_id) override;
  void step_over(uint32_t thread_id) override;
  void step_out(uint32_t thread_id) override;

  bool set_breakpoint(const std::string& file, int line) override;
  void clear_breakpoint(const std::string& file, int line) override;

  std::vector<thread_info> get_threads() override;
  std::vector<frame_info> get_callstack(uint32_t thread_id) override;
  std::vector<watch_entry> evaluate(uint32_t frame_id, const std::vector<std::string>& exprs) override;

  void on_stop(stop_callback cb) override;
  void on_log(log_callback cb) override;

 private:
  struct uv_state; ///< libuv handles; opaque here, defined in the .cpp.

  /// @brief `handle->data` -> owning backend. Every libuv handle in
  ///        `uv_state` stores `this` in `.data`.
  static python_debug_backend* backend_of(void* handle_data);

  /// @brief Outcome of one DAP request: the `response` message plus a
  ///        decoded success flag / error message.
  struct dap_response {
    bool ok{false};
    dap_json body;        ///< `response.body` (`{}` if absent / on error).
    std::string message;  ///< `response.message` when `ok` is false.
  };

  /// @brief Sends a DAP request and blocks (bounded by @p timeout_ms) for
  ///        its matching `response`. Serialised by `send_mtx_`; safe from
  ///        any thread except the loop thread.
  dap_response request(const std::string& command, dap_json arguments = dap_json(nullptr),
                       int timeout_ms = 8000);
  /// @brief `request()` body without taking `send_mtx_` — for use inside
  ///        `do_handshake()`, which holds it across the whole exchange.
  dap_response request_locked(const std::string& command, dap_json arguments, int timeout_ms);

  /// @brief (Re)sends the full breakpoint list for @p file (DAP replaces a
  ///        source's breakpoints wholesale per `setBreakpoints`). Returns
  ///        whether @p want_line came back `verified` (true when
  ///        @p want_line is 0, i.e. a pure clear).
  bool resend_breakpoints(const std::string& file, int want_line);

  // ── loop thread ───────────────────────────────────────────────────────
  void loop_thread_main();
  void on_loop_wakeup();                 ///< uv_async callback: flush writes / shutdown.
  void on_socket_bytes(const char* data, size_t n);
  void handle_dap_message(const dap_json& msg);
  void begin_connect();                  ///< Kick off (or retry) the TCP connect.
  void on_connect_result(int status);

  // ── dispatch thread ──────────────────────────────────────────────────
  void dispatch_thread_main();
  void queue_stop(uint32_t thread_id, std::string dap_reason);
  void queue_log(std::string text, std::string level);

  // ── helpers ──────────────────────────────────────────────────────────
  static std::string discover_python();
  bool verify_debugpy(std::string& err) const;
  static int pick_free_port();
  bool do_handshake(uint32_t pid);
  void teardown();
  int next_seq() {
    return seq_.fetch_add(1, std::memory_order_relaxed);
  }
  void enqueue_write(std::string framed);

  // ── configuration ────────────────────────────────────────────────────
  std::string python_;
  std::string connect_host_;
  int connect_port_{0};
  bool use_adapter_{true};
  int adapter_port_{0};

  // ── threads / libuv ──────────────────────────────────────────────────
  std::unique_ptr<uv_state> uv_;
  std::thread loop_thread_;
  std::thread dispatch_thread_;

  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> attached_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> pause_requested_{false};
  // True once the loop thread has initialised its async/timer/tcp handles
  // and entered uv_run(); gates teardown()'s cross-thread uv_async_send so
  // it never fires at an uninitialised handle (adapter-spawn-failure path).
  std::atomic<bool> loop_ready_{false};

  // connect handshake gate (loop thread -> attach()).
  std::mutex connect_mtx_;
  std::condition_variable connect_cv_;
  bool connect_done_{false};
  bool connect_ok_{false};

  dap_message_reader reader_;

  // outbound write queue (any thread -> loop thread).
  std::mutex write_mtx_;
  std::deque<std::string> write_queue_;

  // in-flight request/response handshake.
  std::mutex send_mtx_;
  std::mutex reply_mtx_;
  std::condition_variable reply_cv_;
  std::atomic<int> seq_{1};
  int pending_seq_{0};
  bool reply_ready_{false};
  bool conn_gone_{false};
  dap_json pending_response_;

  // 'initialized' event gate (loop thread -> do_handshake()).
  std::mutex init_mtx_;
  std::condition_variable init_cv_;
  bool initialized_event_{false};

  // 'attach' response gate: its response is delayed behind
  // configurationDone, so do_handshake() tracks it out-of-band.
  std::atomic<int> attach_seq_{0};
  std::mutex attach_mtx_;
  std::condition_variable attach_cv_;
  bool attach_replied_{false};
  bool attach_ok_{false};
  std::string attach_msg_;

  std::atomic<uint32_t> primary_thread_{0};

  // "file" -> set of breakpoint lines (DAP setBreakpoints is per-source).
  bison::synchronized<std::map<std::string, std::set<int>>> breakpoints_;
  // Call Stack row index (as handed to the server) -> real DAP frame id,
  // from the last get_callstack(); evaluate() maps back through this.
  bison::synchronized<std::map<uint32_t, int64_t>> frame_ids_;

  bison::synchronized<stop_callback> on_stop_cb_;
  bison::synchronized<log_callback> on_log_cb_;

  struct dispatch_item {
    bool is_stop{false};
    uint32_t thread_id{};
    std::string reason; ///< raw DAP stop reason (mapped on the dispatch thread).
    std::string log_text;
    std::string log_level;
  };
  std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::deque<dispatch_item> dispatch_queue_;
};

} // namespace bdg::wish::dbg
