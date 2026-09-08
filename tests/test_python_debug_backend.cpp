// MIT License © 2026 Binary Dice Games
/// @file test_python_debug_backend.cpp
/// @brief Tests for python_debug_backend — the `debugpy` / Debug Adapter
///        Protocol backend selected by `wish client --run=dbg -- --backend
///        python`.
///
/// Two groups:
///  - **fake server**: drives the backend's DAP client against
///    tests/fixtures/fake_dap_server.py (a canned DAP responder, needs no
///    `debugpy`) — covers the socket plumbing, Content-Length framing, the
///    delayed-`attach`-response handshake, and `stopped` -> on_stop
///    dispatch. Skipped only when there is no `python3`.
///  - **live**: connect-mode attach to a real `debugpy` server started by
///    tests/fixtures/dbg_fixture.py; `GTEST_SKIP`s when `debugpy` is not
///    installed (same graceful degradation as test_posix_debug_backend
///    without `gdb`).
///
/// UNIX-only (see tests/CMakeLists.txt) — the child-spawn helpers here use
/// POSIX `posix_spawn`; the protocol layer itself is covered on every
/// platform by test_dap_protocol.cpp.
#include "modules/bdg/dev/dbg/client/python_debug_backend.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bdg::wish::dbg {
namespace {

// ── helpers ──────────────────────────────────────────────────────────────

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

const char* python_bin() {
  return on_path("python3") ? "python3" : (on_path("python") ? "python" : nullptr);
}

bool have_debugpy() {
  const char* py = python_bin();
  if (!py)
    return false;
  std::string cmd = std::string(py) + " -c 'import debugpy' >/dev/null 2>&1";
  return std::system(cmd.c_str()) == 0;
}

int pick_free_port() {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(s);
  return port;
}

pid_t spawn(const std::vector<std::string>& args, int stdout_fd = -1) {
  std::vector<char*> argv;
  for (const auto& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (stdout_fd >= 0)
    posix_spawn_file_actions_adddup2(&fa, stdout_fd, STDOUT_FILENO);

  pid_t pid = -1;
  int rc = ::posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  return rc == 0 ? pid : -1;
}

void kill_and_reap(pid_t pid) {
  if (pid <= 0)
    return;
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

// Bounded queue of stop_events, same shape as test_posix_debug_backend.cpp.
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

std::string fixture_py() {
  return WISH_DBG_PY_FIXTURE;
}
std::string fake_server_py() {
  return WISH_FAKE_DAP_SERVER;
}

// ── fake-server tests ────────────────────────────────────────────────────

class PythonDebugBackendFakeServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!python_bin())
      GTEST_SKIP() << "no python3 on PATH";
    port_ = pick_free_port();
    server_ = spawn({python_bin(), fake_server_py(), std::to_string(port_)});
    ASSERT_GT(server_, 0);
  }
  void TearDown() override {
    kill_and_reap(server_);
  }
  int port_{0};
  pid_t server_{-1};
};

TEST_F(PythonDebugBackendFakeServerTest, FullScriptedSession) {
  python_debug_backend backend("127.0.0.1:" + std::to_string(port_));
  stop_event_queue stops;
  std::atomic<int> logs{0};
  backend.on_stop([&](const stop_event& ev) { stops.push(ev); });
  backend.on_log([&](const std::string&, const std::string&) { ++logs; });

  ASSERT_TRUE(backend.attach(0)) << "connect-mode attach to fake DAP server failed";

  // Setting the breakpoint is acknowledged as verified, and (as with a real
  // running debuggee) the server then fires `stopped(breakpoint)`; the
  // backend resolves the location via stackTrace and delivers a stop_event.
  EXPECT_TRUE(backend.set_breakpoint("/tmp/fake_fixture.py", 17));
  stop_event bp;
  ASSERT_TRUE(stops.pop(bp, std::chrono::seconds(10)));
  EXPECT_EQ(bp.reason, "breakpoint");
  EXPECT_EQ(bp.thread_id, 1u);
  EXPECT_EQ(bp.line, 17);
  EXPECT_NE(bp.file.find("fake_fixture.py"), std::string::npos);

  auto threads = backend.get_threads();
  ASSERT_EQ(threads.size(), 1u);
  EXPECT_EQ(threads[0].id, 1u);
  EXPECT_EQ(threads[0].state, "suspended");
  EXPECT_EQ(threads[0].current_function, "inner_function");

  auto frames = backend.get_callstack(1);
  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].function, "inner_function");
  EXPECT_EQ(frames[1].function, "outer_function");
  EXPECT_EQ(frames[0].line, 17);

  auto watch = backend.evaluate(0, {"x"});
  ASSERT_EQ(watch.size(), 1u);
  EXPECT_EQ(watch[0].name, "x");
  EXPECT_EQ(watch[0].value, "3");
  EXPECT_EQ(watch[0].type, "int");

  backend.step_into(1);
  stop_event step;
  ASSERT_TRUE(stops.pop(step, std::chrono::seconds(10)));
  EXPECT_EQ(step.reason, "step");

  backend.detach();
}

TEST_F(PythonDebugBackendFakeServerTest, PauseReportsPauseReason) {
  python_debug_backend backend("127.0.0.1:" + std::to_string(port_));
  stop_event_queue stops;
  backend.on_stop([&](const stop_event& ev) { stops.push(ev); });

  ASSERT_TRUE(backend.attach(0)); // target "running" after a connect-mode attach

  backend.pause();
  stop_event paused;
  ASSERT_TRUE(stops.pop(paused, std::chrono::seconds(10)));
  EXPECT_EQ(paused.reason, "pause");

  backend.detach();
}

// ── standalone tests ─────────────────────────────────────────────────────

TEST(PythonDebugBackendStandaloneTest, ConnectToNothingFailsCleanly) {
  // Nothing is listening on this port — attach must fail (and log), not hang.
  python_debug_backend backend("127.0.0.1:1"); // port 1: never a debugpy server
  std::string last_level;
  backend.on_log([&](const std::string&, const std::string& level) { last_level = level; });

  EXPECT_FALSE(backend.attach(0));
  EXPECT_EQ(last_level, "error");
}

// ── live tests (need debugpy) ────────────────────────────────────────────

class PythonDebugBackendLiveTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!have_debugpy())
      GTEST_SKIP() << "debugpy not installed (pip install debugpy)";

    int pipefd[2];
    ASSERT_EQ(::pipe(pipefd), 0);
    fixture_ = spawn({python_bin(), "-Xfrozen_modules=off", fixture_py()}, pipefd[1]);
    ::close(pipefd[1]);
    ASSERT_GT(fixture_, 0);

    // Read the "PORT <n>" line the fixture prints once debugpy is listening.
    std::string line;
    char c = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      ssize_t n = ::read(pipefd[0], &c, 1);
      if (n <= 0)
        break;
      if (c == '\n')
        break;
      line += c;
    }
    ::close(pipefd[0]);
    ASSERT_EQ(line.rfind("PORT ", 0), 0u) << "fixture did not report a port: '" << line << "'";
    port_ = std::atoi(line.c_str() + 5);
    ASSERT_GT(port_, 0);
  }
  void TearDown() override {
    kill_and_reap(fixture_);
  }
  int port_{0};
  pid_t fixture_{-1};
};

TEST_F(PythonDebugBackendLiveTest, AttachBreakpointStepInspect) {
  python_debug_backend backend("127.0.0.1:" + std::to_string(port_));
  stop_event_queue stops;
  backend.on_stop([&](const stop_event& ev) { stops.push(ev); });

  ASSERT_TRUE(backend.attach(0)) << "connect-mode attach to live debugpy failed";

  // The target runs freely after a connect-mode attach; the breakpoint it
  // hits on its own is what stops it (no resume() needed here).
  ASSERT_TRUE(backend.set_breakpoint(fixture_py(), 28));

  stop_event bp;
  ASSERT_TRUE(stops.pop(bp, std::chrono::seconds(20)));
  EXPECT_EQ(bp.reason, "breakpoint");
  EXPECT_EQ(bp.line, 28);
  EXPECT_NE(bp.file.find("dbg_fixture.py"), std::string::npos);

  auto frames = backend.get_callstack(bp.thread_id);
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames[0].function, "inner_function");
  EXPECT_GT(frames.size(), 1u);

  auto watch = backend.evaluate(0, {"x"});
  ASSERT_EQ(watch.size(), 1u);
  EXPECT_EQ(watch[0].name, "x");
  EXPECT_FALSE(watch[0].value.empty());

  backend.step_over(bp.thread_id);
  stop_event step;
  ASSERT_TRUE(stops.pop(step, std::chrono::seconds(20)));
  EXPECT_EQ(step.reason, "step");

  backend.clear_breakpoint(fixture_py(), 28);
  backend.detach();
}

} // namespace
} // namespace bdg::wish::dbg
