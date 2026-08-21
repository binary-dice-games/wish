// MIT License © 2025 Binary Dice Games
/// @file process_control_linux.cpp
/// @brief Linux/MSYS2 implementation of process_control.hpp (signals,
/// setpriority/sched_setaffinity, and /proc-based details lookup).
#include "process_control.hpp"

#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace bdg::wish {

namespace {

namespace fs = std::filesystem;

process_action_result signal_result(int rc) {
  process_action_result r;
  if (rc != 0) {
    r.success = false;
    r.error = std::strerror(errno);
  }
  return r;
}

std::string read_link(const std::string& path) {
  char buf[4096];
  ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n <= 0)
    return {};
  buf[n] = '\0';
  return std::string(buf);
}

std::string read_cmdline(int pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
  if (!in.is_open())
    return {};
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  while (!raw.empty() && raw.back() == '\0')
    raw.pop_back();
  for (auto& c : raw)
    if (c == '\0')
      c = ' ';
  return raw;
}

std::vector<int> read_affinity_cores(int pid) {
  std::vector<int> cores;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(pid, sizeof(set), &set) != 0)
    return cores;
  for (int i = 0; i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &set))
      cores.push_back(i);
  return cores;
}

// Splits /proc/<pid>/stat after the last ')' (comm may contain spaces or
// parentheses) and returns the space-separated fields that follow, same
// technique as process_info_linux.cpp's read_proc_pid_stat().
bool read_stat_fields(int pid, std::vector<std::string>& tokens) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  if (!in.is_open())
    return false;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  auto close_paren = content.rfind(')');
  if (close_paren == std::string::npos || close_paren + 2 >= content.size())
    return false;
  std::istringstream iss(content.substr(close_paren + 2));
  std::string tok;
  while (iss >> tok)
    tokens.push_back(tok);
  return true;
}

double read_system_uptime_seconds() {
  std::ifstream in("/proc/uptime");
  double uptime = 0.0;
  if (in.is_open())
    in >> uptime;
  return uptime;
}

std::string format_local_time(std::time_t t) {
  char buf[64];
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf) == 0)
    return {};
  return buf;
}

} // namespace

process_action_result kill_process(int pid) {
  return signal_result(kill(pid, SIGKILL));
}

process_action_result pause_process(int pid) {
  return signal_result(kill(pid, SIGSTOP));
}

process_action_result resume_process(int pid) {
  return signal_result(kill(pid, SIGCONT));
}

process_action_result set_process_priority(int pid, int nice_value) {
  errno = 0;
  return signal_result(setpriority(PRIO_PROCESS, pid, nice_value));
}

process_action_result set_process_affinity(int pid, const std::vector<int>& cores) {
  process_action_result r;
  if (cores.empty()) {
    r.success = false;
    r.error = "affinity core list must not be empty";
    return r;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int core : cores)
    if (core >= 0 && core < CPU_SETSIZE)
      CPU_SET(core, &set);
  return signal_result(sched_setaffinity(pid, sizeof(set), &set));
}

process_details get_process_details(int pid) {
  process_details d;
  d.pid = pid;

  const std::string proc_dir = "/proc/" + std::to_string(pid);
  if (!fs::exists(proc_dir)) {
    d.error = "No such process";
    return d;
  }

  std::vector<std::string> stat_fields;
  if (read_stat_fields(pid, stat_fields)) {
    // tokens[i] is field (i+3) -- tokens[0] is state (field 3), so ppid
    // (field 4) is tokens[1]; nice is field 19 (tokens[16]); starttime is
    // field 22 (tokens[19]). Matches process_info_linux.cpp's own
    // read_proc_pid_stat() indexing (which starts one field later, at
    // state, since it never needs ppid).
    if (stat_fields.size() > 1)
      d.ppid = std::atoi(stat_fields[1].c_str());
    if (stat_fields.size() > 16)
      d.nice = std::atoi(stat_fields[16].c_str());
    if (stat_fields.size() > 19) {
      long clk_tck = sysconf(_SC_CLK_TCK);
      if (clk_tck > 0) {
        double start_seconds = std::strtod(stat_fields[19].c_str(), nullptr) / static_cast<double>(clk_tck);
        double uptime = read_system_uptime_seconds();
        double age_seconds = uptime - start_seconds;
        std::time_t start_wall = std::time(nullptr) - static_cast<std::time_t>(age_seconds);
        d.start_time = format_local_time(start_wall);
      }
    }
  }

  std::ifstream status_in(proc_dir + "/status");
  std::string line;
  uid_t uid = static_cast<uid_t>(-1);
  while (status_in.is_open() && std::getline(status_in, line)) {
    if (line.rfind("Threads:", 0) == 0)
      d.thread_count = std::atoi(line.substr(8).c_str());
    else if (line.rfind("Uid:", 0) == 0) {
      std::istringstream iss(line.substr(4));
      iss >> uid;
    }
  }
  if (uid != static_cast<uid_t>(-1)) {
    if (struct passwd* pw = getpwuid(uid))
      d.user = pw->pw_name;
    else
      d.user = std::to_string(uid);
  }

  d.exe_path = read_link(proc_dir + "/exe");
  d.cwd = read_link(proc_dir + "/cwd");
  d.cmdline = read_cmdline(pid);
  if (d.cmdline.empty())
    d.cmdline = "[" + fs::path(d.exe_path).filename().string() + "]";
  d.affinity_cores = read_affinity_cores(pid);
  d.found = true;
  return d;
}

} // namespace bdg::wish
