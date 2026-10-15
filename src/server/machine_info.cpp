// MIT License © 2026 Binary Dice Games
/// @file machine_info.cpp
/// @brief machine_info implementation: POSIX (Linux) and Windows sources.
#include <server/machine_info.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace bdg::wish {

namespace {

std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string{v} : std::string{};
}

#if !defined(_WIN32)
// Value in kB of the "<key>:" line of a /proc file (meminfo, self/status),
// in bytes; 0 when the file or line is missing (non-Linux POSIX systems).
uint64_t proc_kb_field(const char* file, const std::string& key) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.compare(0, key.size(), key) == 0 && line.size() > key.size() && line[key.size()] == ':') {
      std::istringstream ss(line.substr(key.size() + 1));
      uint64_t kb = 0;
      ss >> kb;
      return kb * 1024u;
    }
  }
  return 0;
}
#endif

} // namespace

machine_info query_machine_info() {
  machine_info m;
  m.cpu_count = std::thread::hardware_concurrency();
#if defined(_WIN32)
  char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD size = sizeof(name);
  if (GetComputerNameA(name, &size))
    m.hostname.assign(name, size);
  m.os = "Windows";
  m.user = env_or_empty("USERNAME");
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (GlobalMemoryStatusEx(&mem)) {
    m.total_memory = mem.ullTotalPhys;
    m.available_memory = mem.ullAvailPhys;
  }
  PROCESS_MEMORY_COUNTERS pmc{};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    m.process_memory = pmc.WorkingSetSize;
  m.pid = GetCurrentProcessId();
#else
  char name[256] = {};
  if (gethostname(name, sizeof(name) - 1) == 0)
    m.hostname = name;
  struct utsname u {};
  if (uname(&u) == 0)
    m.os = std::string{u.sysname} + " " + u.release + " (" + u.machine + ")";
  m.user = env_or_empty("USER");
  m.total_memory = proc_kb_field("/proc/meminfo", "MemTotal");
  m.available_memory = proc_kb_field("/proc/meminfo", "MemAvailable");
  m.process_memory = proc_kb_field("/proc/self/status", "VmRSS");
  m.pid = static_cast<uint64_t>(getpid());
  double load[1] = {};
  if (getloadavg(load, 1) == 1)
    m.load_average = load[0];
#endif
  return m;
}

std::string format_bytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  if (bytes < 1024u)
    return std::to_string(bytes) + " B";
  double v = static_cast<double>(bytes);
  int unit = 0;
  while (v >= 1024.0 && unit < 4) {
    v /= 1024.0;
    ++unit;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", v, kUnits[unit]);
  return buf;
}

std::string format_duration(std::chrono::seconds d) {
  long long s = d.count() < 0 ? 0 : static_cast<long long>(d.count());
  const long long days = s / 86400, hours = s / 3600 % 24, minutes = s / 60 % 60, seconds = s % 60;
  char buf[48];
  if (days > 0)
    std::snprintf(buf, sizeof(buf), "%lldd %02lldh", days, hours);
  else if (hours > 0)
    std::snprintf(buf, sizeof(buf), "%lldh %02lldm", hours, minutes);
  else if (minutes > 0)
    std::snprintf(buf, sizeof(buf), "%lldm %02llds", minutes, seconds);
  else
    std::snprintf(buf, sizeof(buf), "%llds", seconds);
  return buf;
}

} // namespace bdg::wish
