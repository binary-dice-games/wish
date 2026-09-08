// MIT License © 2026 Binary Dice Games
/// @file posix_debug_backend.hpp
/// @brief Linux/macOS implementation of debug_backend, driving a
///        `gdb --interpreter=mi` (or `lldb-mi`) child process.
///
/// The Windows backend (`win32_debug_backend`) talks to the OS debug API
/// directly. There is no equally uniform native equivalent across Linux and
/// macOS -- Linux is `ptrace(2)` + DWARF, macOS is Mach task ports + DWARF
/// + code-signing entitlements -- so this backend instead does what the
/// rest of this repo's tool modules do with their underlying capability
/// (`docker`/`git`/`kubectl` shell out to the real CLI): it spawns the
/// platform's real debugger as a child and drives it through its stable
/// machine-interface protocol (GDB/MI, also spoken by `lldb-mi`). One code
/// path serves both platforms; the debugger binary is discovered on `PATH`
/// (`gdb`, then `lldb-mi`) or forced via the `WISH_DBG_DEBUGGER` environment
/// variable.
///
/// Not compiled on Windows (`win32_debug_backend` is selected there); the
/// whole file `#if`s out so a Windows build of the dbg module still links.
#pragma once

#if !defined(_WIN32)

#include "debug_backend.hpp"
#include "mi_parser.hpp"

#include "src/bison/bison_sync.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish::dbg {

/// @brief debug_backend backed by a child `gdb`/`lldb-mi` process.
///
/// Threading model:
/// - The **reader thread** owns the read end of the debugger's stdout pipe.
///   It parses each MI line (`mi_parser.hpp`) and routes it: `^`/result
///   records complete the one in-flight `send_command()` call; `*stopped`
///   and stream records are pushed to `dispatch_queue_`.
/// - The **dispatch thread** drains `dispatch_queue_` and invokes the
///   `on_stop`/`on_log` callbacks. It runs separately from the reader
///   thread precisely so a callback (`dbg_source::handle_stop`) can call
///   back into `get_threads()`/`get_callstack()`/`evaluate()` -- each of
///   which issues its own `send_command()` -- without deadlocking against
///   the thread that must service that command's reply.
/// - Public methods are called from the RMI dispatch thread (via
///   `dbg_source`). `send_command()` serialises them with `send_mtx_`.
class posix_debug_backend : public debug_backend {
 public:
  posix_debug_backend() = default;
  ~posix_debug_backend() override;

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
  /// @brief Result of one MI command: the terminating result record plus
  ///        any `~`/`@`/`&` stream text emitted while it was in flight.
  struct command_result {
    bool ok{false}; ///< A `^done`/`^running`/`^connected`/`^exit` (not `^error`, not a timeout).
    mi_record record;
    std::string streams; ///< Concatenated unescaped stream-record text.
  };

  /// @brief Sends `<token><command>\n` to the debugger and blocks (bounded
  ///        by @p timeout_ms) for its result record. Serialised by
  ///        `send_mtx_`; safe from any thread except the reader thread.
  command_result send_command(const std::string& command, int timeout_ms = 8000);

  /// @brief Spawns the debugger child, wiring `child_stdin_` / `child_stdout_`.
  ///        Returns false if the binary could not be found or `fork`/`exec`
  ///        failed.
  bool spawn_debugger();

  void reader_loop();   ///< Body of `reader_thread_`.
  void dispatch_loop(); ///< Body of `dispatch_thread_`.

  /// @brief Turns a `*stopped` record into a `stop_event` and queues it.
  void handle_stopped(const mi_record& rec);
  /// @brief Common shutdown: closes pipes, joins threads, reaps the child.
  void teardown();

  /// @brief Discovers the debugger binary: `$WISH_DBG_DEBUGGER`, else the
  ///        first of `gdb` / `lldb-mi` found on `PATH`. Empty if none.
  static std::string discover_debugger();

  // ── Child process / pipes ──────────────────────────────────────────────
  int child_pid_{-1};
  int child_stdin_{-1};  ///< Write end: MI commands to the debugger.
  int child_stdout_{-1}; ///< Read end: MI records from the debugger.
  std::string debugger_path_;

  std::thread reader_thread_;
  std::thread dispatch_thread_;
  std::atomic<bool> shutting_down_{false};
  std::atomic<bool> attached_{false};
  std::atomic<bool> running_{false}; ///< True between `*running` and `*stopped`.
  bool reported_attach_{false};      ///< First post-attach stop is reported as "attach".
  std::atomic<bool> pause_requested_{false};

  std::atomic<uint32_t> last_stop_thread_{0};

  // ── In-flight command handshake (reader thread <-> send_command) ───────
  std::mutex send_mtx_; ///< One outstanding command at a time.
  std::mutex reply_mtx_;
  std::condition_variable reply_cv_;
  std::atomic<uint64_t> token_seq_{0};
  std::atomic<bool> command_in_flight_{false};
  bool reply_ready_{false};
  bool reader_gone_{false}; ///< Set when the reader hits EOF / the child dies.
  mi_record pending_record_;
  std::string stream_accum_; ///< Stream text captured during the in-flight command.

  // ── Async event dispatch (reader thread -> dispatch thread) ────────────
  struct dispatch_item {
    bool is_stop{false};
    stop_event stop;
    std::string log_text;
    std::string log_level;
  };
  std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::deque<dispatch_item> dispatch_queue_;

  // ── Breakpoints: "file:line" -> MI breakpoint number ───────────────────
  bison::synchronized<std::map<std::string, int>> breakpoints_;

  bison::synchronized<stop_callback> on_stop_cb_;
  bison::synchronized<log_callback> on_log_cb_;
};

} // namespace bdg::wish::dbg

#endif // !defined(_WIN32)
