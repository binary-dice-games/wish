// MIT License © 2026 Binary Dice Games
/// @file test_win32_debug_backend.cpp
/// @brief Tests for win32_debug_backend (PLAN.md Step 4) against the real
///        dbg_fixture executable, per the exact assertions in DESIGN.md §9:
///        attach, set a breakpoint, resume, assert the stop event reports
///        the expected file/line; single-step; detach and confirm the
///        process continues to completion with no leftover INT3.
///
/// Windows-only (see tests/CMakeLists.txt's `if(WIN32)` guard) since
/// win32_debug_backend itself compiles to nothing elsewhere.
#include "modules/bdg/dev/dbg/client/win32_debug_backend.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <deque>
#include <mutex>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace bdg::wish::dbg {
namespace {

// Bounded queue of stop_events delivered via on_stop(), off whatever thread
// the backend's debug thread happens to be -- mirrors the wait pattern in
// tests/test_client.cpp's ClientTest.OnDisconnectedFiresWhenServerClosesConnection
// (a plain mutex/condition_variable, bounded by a timeout so a regression
// here fails this test instead of hanging the whole suite).
class stop_event_queue {
 public:
  void push(const stop_event& ev) {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      events_.push_back(ev);
    }
    cv_.notify_all();
  }

  // Waits up to 10s for the next stop_event; fails the calling test if none
  // arrives in time.
  bool pop(stop_event& out) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_.wait_for(lk, std::chrono::seconds(10), [&] { return !events_.empty(); }))
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

std::string fixture_path() {
  return WISH_DBG_FIXTURE_PATH;
}

// Launches dbg_fixture.exe (not suspended, not under the debug API) and
// returns its PID -- win32_debug_backend::attach() exercises the
// DebugActiveProcess-on-an-already-running-process path, matching the
// module's real Attach flow (DESIGN.md §4).
uint32_t launch_fixture(PROCESS_INFORMATION& pi) {
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  std::string cmd = fixture_path();
  BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  if (!ok)
    return 0;
  return pi.dwProcessId;
}

TEST(Win32DebugBackendTest, AttachSetBreakpointResumeHitsExpectedLine) {
  PROCESS_INFORMATION pi{};
  uint32_t pid = launch_fixture(pi);
  ASSERT_NE(pid, 0u);

  win32_debug_backend backend;
  stop_event_queue events;
  backend.on_stop([&](const stop_event& ev) { events.push(ev); });

  ASSERT_TRUE(backend.attach(pid));

  // DebugActiveProcess synthesizes an initial CREATE_PROCESS_DEBUG_EVENT,
  // which the backend reports as the "attach" stop (DESIGN.md §4's Attach
  // flow) before anything else runs.
  stop_event attach_ev;
  ASSERT_TRUE(events.pop(attach_ev));
  EXPECT_EQ(attach_ev.reason, "attach");

  ASSERT_TRUE(backend.set_breakpoint("dbg_fixture.cpp", 24));
  backend.resume();

  stop_event bp_ev;
  ASSERT_TRUE(events.pop(bp_ev));
  EXPECT_EQ(bp_ev.reason, "breakpoint");
  EXPECT_EQ(bp_ev.line, 24);
  EXPECT_NE(bp_ev.file.find("dbg_fixture.cpp"), std::string::npos);

  // Single-step from the breakpoint's restored instruction.
  backend.step_into(bp_ev.thread_id);
  stop_event step_ev;
  ASSERT_TRUE(events.pop(step_ev));
  EXPECT_EQ(step_ev.reason, "step");

  backend.clear_breakpoint("dbg_fixture.cpp", 24);
  backend.detach();

  // No leftover INT3: the fixture's bounded loop (20 iterations) must run to
  // real, unassisted completion with its declared exit code, not crash on a
  // patched byte we failed to restore (DESIGN.md §7's detach invariant).
  DWORD wait_result = WaitForSingleObject(pi.hProcess, 10000);
  ASSERT_EQ(wait_result, WAIT_OBJECT_0);
  DWORD exit_code = 0;
  ASSERT_TRUE(GetExitCodeProcess(pi.hProcess, &exit_code));
  EXPECT_EQ(exit_code, 42u);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

TEST(Win32DebugBackendTest, DetachWithoutBreakpointsLetsProcessRunToCompletion) {
  PROCESS_INFORMATION pi{};
  uint32_t pid = launch_fixture(pi);
  ASSERT_NE(pid, 0u);

  win32_debug_backend backend;
  stop_event_queue events;
  backend.on_stop([&](const stop_event& ev) { events.push(ev); });

  ASSERT_TRUE(backend.attach(pid));

  stop_event attach_ev;
  ASSERT_TRUE(events.pop(attach_ev));
  EXPECT_EQ(attach_ev.reason, "attach");

  backend.resume();
  backend.detach();

  DWORD wait_result = WaitForSingleObject(pi.hProcess, 10000);
  ASSERT_EQ(wait_result, WAIT_OBJECT_0);
  DWORD exit_code = 0;
  ASSERT_TRUE(GetExitCodeProcess(pi.hProcess, &exit_code));
  EXPECT_EQ(exit_code, 42u);

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

} // namespace
} // namespace bdg::wish::dbg
