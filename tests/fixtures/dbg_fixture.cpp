// MIT License © 2026 Binary Dice Games
/// @file dbg_fixture.cpp
/// @brief Tiny standalone executable, the attach target for the debugger
///        backend tests (test_win32_debug_backend / test_posix_debug_backend,
///        PLAN.md Steps 4 & 9 / DESIGN.md §9). Built standalone so a test can
///        launch it and attach a debug_backend. inner_function()/outer_function()
///        are non-inlined (built at -Od / -O0) so the debugger sees distinct
///        frames and a real CALL at the call site -- what step_over/step_out
///        exercise. NOTE: the tests hard-code line 24 (breakpoint) and line 29
///        (call site); keep inner_function() at line 23.
#include <chrono>
#include <cstdio>
#include <thread>
#if defined(__linux__)
#include <sys/prctl.h> // PR_SET_PTRACER lets the test's sibling gdb attach.
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY (-1UL)
#endif
#endif
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
int inner_function(int x) {
  int y = x * 2; // Breakpoint line: test sets a breakpoint here.
  return y;
}

int outer_function(int x) {
  int r = inner_function(x);
  return r + 1;
}

int main() {
#if defined(__linux__) && defined(PR_SET_PTRACER)
  // The POSIX backend test spawns gdb as its own child -- a sibling of this
  // process, not an ancestor -- so under the common yama/ptrace_scope=1
  // policy gdb could not otherwise attach. This is a throwaway test
  // fixture, so opting into "anyone may trace me" is fine (and a no-op
  // where the policy is already permissive).
  prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

  // Bounded loop (not infinite): a test detaches partway through and then
  // needs to observe the process reach real, unassisted completion
  // (DESIGN.md §9) with a known exit code -- an infinite loop would force
  // the test to kill it instead, which proves nothing about leftover
  // breakpoints. The count is sized so the process is still comfortably
  // running while a test attaches (the POSIX backend attaches to an
  // already-running process and needs it alive long enough to spawn gdb,
  // attach, and drive a step), yet still exits on its own well inside
  // every test's wait timeout.
  int counter = 0;
  for (int i = 0; i < 100; ++i) {
    counter = outer_function(counter);
    std::printf("dbg_fixture: counter=%d\n", counter);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return 42;
}

#if defined(_MSC_VER)
#pragma optimize("", on)
#endif
