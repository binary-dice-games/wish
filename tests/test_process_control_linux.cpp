// MIT License © 2025 Binary Dice Games
/// @file test_process_control_linux.cpp
/// @brief Exercises process_control_linux.cpp's kill/pause/resume/priority/
/// affinity/details actions against a real, disposable child process.
///
/// Linux-only (guarded in tests/CMakeLists.txt by `if(UNIX)`, matching
/// process_control_linux.cpp's own "Linux/MSYS2" scope) -- these are real
/// signals and scheduler syscalls, not something a portable fake can stand
/// in for.
#include <gtest/gtest.h>

#include "modules/bdg/desktop/process_explorer/client/process_control.hpp"

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <string>
#include <thread>

using namespace bdg::wish;

namespace {

// Reads /proc/<pid>/stat's state character (field 3), the same way
// process_control_linux.cpp's own read_stat_fields() splits after the last
// ')' -- comm may contain spaces/parentheses.
char read_proc_state(pid_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  if (!in.is_open())
    return '\0';
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  auto close_paren = content.rfind(')');
  if (close_paren == std::string::npos || close_paren + 2 >= content.size())
    return '\0';
  return content[close_paren + 2];
}

bool wait_for_state(pid_t pid, char expected) {
  for (int i = 0; i < 200; ++i) {
    if (read_proc_state(pid) == expected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

class ProcessControlLinuxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pid_ = fork();
    ASSERT_GE(pid_, 0) << "fork failed";
    if (pid_ == 0) {
      // Child: block in pause() until signaled -- always alive (state 'S')
      // until the test (or TearDown) kills it.
      for (;;)
        pause();
      _exit(0); // NOLINT: unreachable, but documents intent.
    }
    ASSERT_TRUE(wait_for_state(pid_, 'S')) << "child never reached sleeping state";
  }

  void TearDown() override {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      waitpid(pid_, nullptr, 0);
    }
  }

  pid_t pid_{-1};
};

} // namespace

TEST_F(ProcessControlLinuxTest, KillProcessTerminatesChild) {
  auto result = kill_process(pid_);
  EXPECT_TRUE(result.success) << result.error;

  int status = 0;
  ASSERT_EQ(waitpid(pid_, &status, 0), pid_);
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
  pid_ = -1; // already reaped -- TearDown must not touch it again.
}

TEST_F(ProcessControlLinuxTest, KillNonexistentPidReportsFailure) {
  kill(pid_, SIGKILL);
  waitpid(pid_, nullptr, 0);
  pid_t dead_pid = pid_;
  pid_ = -1;

  auto result = kill_process(dead_pid);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(ProcessControlLinuxTest, PauseThenResumeTogglesStopState) {
  auto pause_result = pause_process(pid_);
  EXPECT_TRUE(pause_result.success) << pause_result.error;
  EXPECT_TRUE(wait_for_state(pid_, 'T')) << "child never reported stopped ('T')";

  auto resume_result = resume_process(pid_);
  EXPECT_TRUE(resume_result.success) << resume_result.error;
  EXPECT_TRUE(wait_for_state(pid_, 'S')) << "child never reported sleeping ('S') again";
}

TEST_F(ProcessControlLinuxTest, SetProcessPriorityChangesNiceValue) {
  auto result = set_process_priority(pid_, 10);
  EXPECT_TRUE(result.success) << result.error;

  errno = 0;
  int nice_value = getpriority(PRIO_PROCESS, pid_);
  ASSERT_EQ(errno, 0) << "getpriority failed";
  EXPECT_EQ(nice_value, 10);
}

TEST_F(ProcessControlLinuxTest, SetProcessAffinityRestrictsToRequestedCore) {
  auto result = set_process_affinity(pid_, {0});
  EXPECT_TRUE(result.success) << result.error;

  cpu_set_t set;
  CPU_ZERO(&set);
  ASSERT_EQ(sched_getaffinity(pid_, sizeof(set), &set), 0);
  EXPECT_TRUE(CPU_ISSET(0, &set));
  for (int i = 1; i < CPU_SETSIZE; ++i)
    EXPECT_FALSE(CPU_ISSET(i, &set)) << "core " << i << " unexpectedly still allowed";
}

TEST_F(ProcessControlLinuxTest, SetProcessAffinityRejectsEmptyCoreList) {
  auto result = set_process_affinity(pid_, {});
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.error.empty());
}

TEST_F(ProcessControlLinuxTest, GetProcessDetailsReturnsBasicFields) {
  auto details = get_process_details(pid_);
  EXPECT_TRUE(details.found);
  EXPECT_EQ(details.pid, pid_);
  EXPECT_EQ(details.ppid, getpid());
  EXPECT_FALSE(details.exe_path.empty());
  EXPECT_FALSE(details.cmdline.empty());
}

TEST_F(ProcessControlLinuxTest, GetProcessDetailsForNonexistentPidReportsNotFound) {
  kill(pid_, SIGKILL);
  waitpid(pid_, nullptr, 0);
  pid_t dead_pid = pid_;
  pid_ = -1;

  auto details = get_process_details(dead_pid);
  EXPECT_FALSE(details.found);
  EXPECT_FALSE(details.error.empty());
}
