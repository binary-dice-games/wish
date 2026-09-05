// MIT License © 2026 Binary Dice Games
/// @file dbg_fixture.cpp
/// @brief Tiny standalone Windows executable used as the attach target for
///        test_win32_debug_backend.cpp (PLAN.md Step 4 / DESIGN.md §9).
///
/// Not part of any wish library or module -- built as its own executable so
/// the test can CreateProcess() it, attach a win32_debug_backend, set a
/// breakpoint on a known line in inner_function(), and single-step, all
/// against real, repeatedly-hittable control flow. Deliberately tiny and
/// dependency-free so its debug-info layout stays simple and predictable.
#include <chrono>
#include <cstdio>
#include <thread>

// Kept as separate, non-inlined functions (see CMakeLists.txt: built at -Od)
// so the debugger sees distinct call frames and a real CALL instruction at
// outer_function()'s call site -- exactly what step_over()'s CALL-length
// decode and step_out()'s return-address unwind need to exercise.
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
  // Bounded loop (not infinite): the test detaches partway through and then
  // needs to observe the process reach real, unassisted completion (DESIGN.md
  // §9) with a known exit code -- an infinite loop would force the test to
  // kill it instead, which proves nothing about leftover INT3s.
  int counter = 0;
  for (int i = 0; i < 20; ++i) {
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
