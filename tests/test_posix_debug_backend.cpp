// MIT License © 2026 Binary Dice Games
/// @file test_posix_debug_backend.cpp
/// @brief Tests for posix_debug_backend against the real dbg_fixture
///        executable, mirroring tests/test_win32_debug_backend.cpp's
///        assertions on the other platform: attach, set a breakpoint,
///        resume, assert the stop event's file/line; step; inspect the
///        thread list / call stack / a local; detach and confirm the
///        process runs to completion on its own.
///
/// UNIX-only (see tests/CMakeLists.txt's `elseif(UNIX)` guard). Skips every
/// case at runtime when no `gdb` / `lldb-mi` is on PATH -- the same way
/// tests/test_docker_process.cpp degrades on a host with no Docker -- so a
/// CI image without a debugger installed reports "skipped", not "failed".
#include "modules/bdg/dev/dbg/client/posix_debug_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bdg::wish::dbg {
namespace {

// ── Helpers ──────────────────────────────────────────────────────────────

std::string fixture_path() {
  return WISH_DBG_FIXTURE_PATH;
}

bool on_path(const char* name) {
  const char* path_env = ::getenv("PATH");
  if (!path_env)
    return false;
  std::string path = path_env;
  size_t start = 0;
  while (start <= path.size()) {
    size_t colon = path.find(':', start);
    std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
    if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0)
      return true;
    if (colon == std::string::npos)
      break;
    start = colon + 1;
  }
  return false;
}

bool have_debugger() {
  if (const char* forced = ::getenv("WISH_DBG_DEBUGGER"); forced && *forced)
    return true;
  return on_path("gdb") || on_path("lldb-mi");
}

// Launches dbg_fixture (running normally, not traced) and returns its pid.
pid_t launch_fixture() {
  std::string path = fixture_path();
  char* argv[] = {path.data(), nullptr};
  pid_t pid = 0;
  if (::posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, environ) != 0)
    return -1;
  return pid;
}

// Waits for the fixture to exit on its own -- use when the test asserts it
// runs to completion after a clean detach.
void reap(pid_t pid) {
  int status = 0;
  ::waitpid(pid, &status, 0);
}

// Kills the fixture and reaps it -- use when the test is done and doesn't
// care about the fixture's own exit (saves ~5s of loop runtime).
void kill_and_reap(pid_t pid) {
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

// Bounded queue of stop_events delivered via on_stop() off the backend's
// dispatch thread -- same pattern as test_win32_debug_backend.cpp.
class stop_event_queue {
 public:
  void push(const stop_event& ev) {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      events_.push_back(ev);
    }
    cv_.notify_all();
  }

  bool pop(stop_event& out, std::chrono::seconds timeout = std::chrono::seconds(15)) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_.wait_for(lk, timeout, [&] { return !events_.empty(); }))
      return false;
    out = events_.front();
    events_.pop_front();
    return true;
  }

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::deque<stop_event> events_;
};

// Fixture that skips when there is no debugger to drive.
class PosixDebugBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!have_debugger())
      GTEST_SKIP() << "no gdb/lldb-mi on PATH";
  }
};

// Attaches, skipping the test (rather than failing it) when the debugger
// reports it is not allowed to trace -- e.g. a hardened CI kernel with
// yama/ptrace_scope >= 2, a container without CAP_SYS_PTRACE, or macOS
// without the right entitlements. A genuine backend regression still
// fails, because attach() would return false with no such log line.
// Defined below `log_sink`.
#define ATTACH_OR_SKIP(backend, pid, sink)                                                                   \
  do {                                                                                                       \
    if (!(backend).attach(static_cast<uint32_t>(pid))) {                                                     \
      std::string lc = (sink).str();                                                                         \
      for (char& c : lc)                                                                                     \
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));                                     \
      if (lc.find("ptrace") != std::string::npos || lc.find("not permitted") != std::string::npos)           \
        GTEST_SKIP() << "kernel/OS forbids ptrace attach: " << (sink).str();                                 \
      FAIL() << "attach() failed: " << (sink).str();                                                         \
    }                                                                                                        \
  } while (0)

// Collects every on_log line so ATTACH_OR_SKIP() can inspect the failure
// reason, and so a test can wait for the debugger to say something.
struct log_sink {
  std::mutex mtx;
  std::string joined;
  void attach(posix_debug_backend& b) {
    b.on_log([this](const std::string& text, const std::string& level) {
      std::lock_guard<std::mutex> lk(mtx);
      joined += level + ": " + text + "\n";
    });
  }
  std::string str() {
    std::lock_guard<std::mutex> lk(mtx);
    return joined;
  }
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(PosixDebugBackendTest, AttachReportsInitialStopThenDetachRunsToCompletion) {
  pid_t pid = launch_fixture();
  ASSERT_GT(pid, 0);
  // Let the fixture get into its loop before we attach.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  {
    posix_debug_backend backend;
    stop_event_queue events;
    log_sink logs;
    logs.attach(backend);
    backend.on_stop([&](const stop_event& ev) { events.push(ev); });

    ATTACH_OR_SKIP(backend, pid, logs);

    stop_event attach_ev;
    ASSERT_TRUE(events.pop(attach_ev));
    EXPECT_EQ(attach_ev.reason, "attach");

    backend.resume();
    backend.detach();
  }

  reap(pid);
}

TEST_F(PosixDebugBackendTest, SetBreakpointResumeHitsExpectedLineAndInspects) {
  pid_t pid = launch_fixture();
  ASSERT_GT(pid, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  {
    posix_debug_backend backend;
    stop_event_queue events;
    log_sink logs;
    logs.attach(backend);
    backend.on_stop([&](const stop_event& ev) { events.push(ev); });

    ATTACH_OR_SKIP(backend, pid, logs);
    stop_event attach_ev;
    ASSERT_TRUE(events.pop(attach_ev));

    ASSERT_TRUE(backend.set_breakpoint("dbg_fixture.cpp", 24));
    backend.resume();

    stop_event bp_ev;
    ASSERT_TRUE(events.pop(bp_ev));
    EXPECT_EQ(bp_ev.reason, "breakpoint");
    EXPECT_EQ(bp_ev.line, 24);
    EXPECT_NE(bp_ev.file.find("dbg_fixture.cpp"), std::string::npos);

    // Threads / call stack are populated while stopped.
    auto threads = backend.get_threads();
    ASSERT_FALSE(threads.empty());

    auto frames = backend.get_callstack(bp_ev.thread_id);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames[0].function, "inner_function");
    EXPECT_NE(frames.size(), 1u); // inner_function <- outer_function <- main ...

    // Watch: the parameter `x` resolves as a simple scalar local read.
    auto entries = backend.evaluate(0, {"x"});
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "x");
    EXPECT_FALSE(entries[0].value.empty());
    EXPECT_NE(entries[0].type.find("int"), std::string::npos);

    // Step from the breakpoint; the stop is reported as a step completion.
    backend.step_into(bp_ev.thread_id);
    stop_event step_ev;
    ASSERT_TRUE(events.pop(step_ev));
    EXPECT_EQ(step_ev.reason, "step");

    backend.clear_breakpoint("dbg_fixture.cpp", 24);
    backend.resume();
    backend.detach();
  }

  kill_and_reap(pid);
}

TEST_F(PosixDebugBackendTest, StepOverStaysInOuterFunction) {
  pid_t pid = launch_fixture();
  ASSERT_GT(pid, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  {
    posix_debug_backend backend;
    stop_event_queue events;
    log_sink logs;
    logs.attach(backend);
    backend.on_stop([&](const stop_event& ev) { events.push(ev); });

    ATTACH_OR_SKIP(backend, pid, logs);
    stop_event attach_ev;
    ASSERT_TRUE(events.pop(attach_ev));

    // Line 29 is outer_function()'s `int r = inner_function(x);` call site.
    ASSERT_TRUE(backend.set_breakpoint("dbg_fixture.cpp", 29));
    backend.resume();

    stop_event bp_ev;
    ASSERT_TRUE(events.pop(bp_ev));
    ASSERT_EQ(bp_ev.reason, "breakpoint");
    ASSERT_EQ(bp_ev.line, 29);

    backend.step_over(bp_ev.thread_id);
    stop_event step_ev;
    ASSERT_TRUE(events.pop(step_ev));
    EXPECT_EQ(step_ev.reason, "step");

    auto frames = backend.get_callstack(step_ev.thread_id);
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames[0].function, "outer_function");

    backend.clear_breakpoint("dbg_fixture.cpp", 29);
    backend.resume();
    backend.detach();
  }

  kill_and_reap(pid);
}

TEST_F(PosixDebugBackendTest, PauseWhileRunningStopsWithPauseReason) {
  pid_t pid = launch_fixture();
  ASSERT_GT(pid, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  {
    posix_debug_backend backend;
    stop_event_queue events;
    log_sink logs;
    logs.attach(backend);
    backend.on_stop([&](const stop_event& ev) { events.push(ev); });

    ATTACH_OR_SKIP(backend, pid, logs);
    stop_event attach_ev;
    ASSERT_TRUE(events.pop(attach_ev));

    backend.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    backend.pause();

    stop_event pause_ev;
    ASSERT_TRUE(events.pop(pause_ev));
    EXPECT_EQ(pause_ev.reason, "pause");

    backend.resume();
    backend.detach();
  }

  kill_and_reap(pid);
}

// Drives a full scripted MI session against the fake debugger (no real
// process, no gdb needed) -- covers spawn / $WISH_DBG_DEBUGGER discovery /
// command-token correlation / stream capture / *stopped -> on_stop
// dispatch / callstack + evaluate parsing / clean detach + teardown.
TEST(PosixDebugBackendScriptedTest, RunsAScriptedMiSessionEndToEnd) {
  ::setenv("WISH_DBG_DEBUGGER", WISH_FAKE_MI_DEBUGGER, 1);

  posix_debug_backend backend;
  stop_event_queue events;
  std::atomic<int> logs{0};
  backend.on_stop([&](const stop_event& ev) { events.push(ev); });
  backend.on_log([&](const std::string&, const std::string&) { ++logs; });

  ASSERT_TRUE(backend.attach(999));

  stop_event attach_ev;
  ASSERT_TRUE(events.pop(attach_ev, std::chrono::seconds(5)));
  EXPECT_EQ(attach_ev.reason, "attach");
  EXPECT_EQ(attach_ev.line, 41);
  EXPECT_EQ(attach_ev.file, "/tmp/dbg_fixture.cpp");

  ASSERT_TRUE(backend.set_breakpoint("dbg_fixture.cpp", 24));
  backend.resume();

  stop_event bp_ev;
  ASSERT_TRUE(events.pop(bp_ev, std::chrono::seconds(5)));
  EXPECT_EQ(bp_ev.reason, "breakpoint");
  EXPECT_EQ(bp_ev.line, 24);
  EXPECT_EQ(bp_ev.thread_id, 1u);

  auto threads = backend.get_threads();
  ASSERT_EQ(threads.size(), 1u);
  EXPECT_EQ(threads[0].id, 1u);
  EXPECT_EQ(threads[0].state, "suspended");
  EXPECT_EQ(threads[0].current_function, "inner_function");

  auto frames = backend.get_callstack(1);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].function, "inner_function");
  EXPECT_EQ(frames[1].function, "outer_function");
  EXPECT_EQ(frames[2].function, "main");
  EXPECT_EQ(frames[0].line, 24);
  EXPECT_EQ(frames[0].file, "/tmp/dbg_fixture.cpp");

  auto entries = backend.evaluate(0, {"x"});
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].name, "x");
  EXPECT_EQ(entries[0].value, "3");
  EXPECT_EQ(entries[0].type, "int");

  backend.step_into(1);
  stop_event step_ev;
  ASSERT_TRUE(events.pop(step_ev, std::chrono::seconds(5)));
  EXPECT_EQ(step_ev.reason, "step");
  EXPECT_EQ(step_ev.line, 25);

  backend.clear_breakpoint("dbg_fixture.cpp", 24);
  backend.detach();

  ::unsetenv("WISH_DBG_DEBUGGER");
}

// Not gated on have_debugger(): most useful precisely when none is present.
TEST(PosixDebugBackendStandaloneTest, AttachWithNoDebuggerBinaryFails) {
  // Force a non-existent debugger; attach must fail cleanly (and log),
  // never hang.
  ::setenv("WISH_DBG_DEBUGGER", "/nonexistent/definitely-not-a-debugger", 1);

  posix_debug_backend backend;
  std::string last_level;
  backend.on_log([&](const std::string&, const std::string& level) { last_level = level; });

  EXPECT_FALSE(backend.attach(4242));
  EXPECT_EQ(last_level, "error");

  ::unsetenv("WISH_DBG_DEBUGGER");
}

} // namespace
} // namespace bdg::wish::dbg
