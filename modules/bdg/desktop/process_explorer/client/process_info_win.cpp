// MIT License © 2025 Binary Dice Games
/// @file process_info_win.cpp
/// @brief Windows implementation of process_info_source.
///
/// Implements the same sampling interface as process_info_linux.cpp but
/// using Win32 APIs (GetSystemTimes, Toolhelp32Snapshot, GetProcessTimes,
/// GetProcessMemoryInfo, GlobalMemoryStatusEx, QueryFullProcessImageName).
#include "process_info.hpp"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace bdg::wish {

namespace {

uint64_t filetime_to_u64(const FILETIME& ft) {
  return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void read_system_times(uint64_t& idle, uint64_t& total) {
  FILETIME idleFT{}, kernelFT{}, userFT{};
  if (!GetSystemTimes(&idleFT, &kernelFT, &userFT)) {
    idle = total = 0;
    return;
  }
  uint64_t k = filetime_to_u64(kernelFT);
  uint64_t u = filetime_to_u64(userFT);
  uint64_t i = filetime_to_u64(idleFT);
  // kernel includes idle; total = kernel + user
  total = k + u;
  idle = i;
}

uint64_t now_filetime_u64() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return filetime_to_u64(ft);
}

uint64_t get_num_logical_processors() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<uint64_t>(si.dwNumberOfProcessors);
}

uint64_t get_process_times_u64(HANDLE h) {
  FILETIME creation, exit, kernel, user;
  if (!GetProcessTimes(h, &creation, &exit, &kernel, &user))
    return 0;
  return filetime_to_u64(kernel) + filetime_to_u64(user);
}

uint64_t get_process_rss(HANDLE h) {
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
    return static_cast<uint64_t>(pmc.WorkingSetSize);
  return 0;
}

std::string query_process_command(HANDLE h) {
  std::wstring buf;
  DWORD size = 0;
  // QueryFullProcessImageName requires a buffer; call once to get reasonable size.
  wchar_t tmp[MAX_PATH];
  DWORD len = MAX_PATH;
  if (QueryFullProcessImageNameW(h, 0, tmp, &len)) {
    std::wstring ws(tmp, len);
    // convert to narrow
    int needed = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
      std::string out(needed, '\0');
      WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
      return out;
    }
  }
  return {};
}

} // namespace

struct process_info_source::impl {
  bool has_prev{false};
  uint64_t prev_total{0};
  uint64_t prev_idle{0};
  uint64_t prev_wall{0};
  std::unordered_map<int, uint64_t> prev_proc_times;

  system_snapshot sample() {
    system_snapshot snap;

    uint64_t cur_idle = 0, cur_total = 0;
    read_system_times(cur_idle, cur_total);

    if (has_prev && cur_total > prev_total) {
      uint64_t delta_total = cur_total - prev_total;
      uint64_t delta_idle = cur_idle > prev_idle ? cur_idle - prev_idle : 0;
      if (delta_total == 0)
        snap.system.cpu_percent = 0.0;
      else
        snap.system.cpu_percent = 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
    } else {
      snap.system.cpu_percent = 0.0;
    }

    // Per-core: Windows doesn't expose per-core cumulative times via GetSystemTimes.
    // Provide a vector sized to number of logical processors filled with the
    // overall CPU percentage as a reasonable approximation.
    size_t num_cores = static_cast<size_t>(get_num_logical_processors());
    snap.system.per_core_percent.assign(num_cores, snap.system.cpu_percent);

    // Memory
    MEMORYSTATUSEX msx;
    msx.dwLength = sizeof(msx);
    if (GlobalMemoryStatusEx(&msx)) {
      snap.system.mem_total_bytes = msx.ullTotalPhys;
      snap.system.mem_used_bytes = msx.ullTotalPhys > msx.ullAvailPhys ? (msx.ullTotalPhys - msx.ullAvailPhys) : 0;
    } else {
      snap.system.mem_total_bytes = snap.system.mem_used_bytes = 0;
    }

    uint64_t cur_wall = now_filetime_u64();
    uint64_t delta_wall = (has_prev && cur_wall > prev_wall) ? (cur_wall - prev_wall) : 0;

    std::unordered_map<int, uint64_t> next_prev_proc_times;

    // Enumerate processes via Toolhelp32Snapshot
    HANDLE snap_h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap_h != INVALID_HANDLE_VALUE) {
      PROCESSENTRY32W pe{};
      pe.dwSize = sizeof(pe);
      if (Process32FirstW(snap_h, &pe)) {
        do {
          int pid = static_cast<int>(pe.th32ProcessID);
          process_sample ps;
          ps.pid = pid;
          // name from pe32
          std::wstring wname(pe.szExeFile);
          int needed = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, nullptr, 0, nullptr, nullptr);
          if (needed > 0) {
            std::string name(needed - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1, name.data(), needed, nullptr, nullptr);
            ps.name = name;
          }
          ps.state = '?';

          HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
          if (ph != NULL) {
            uint64_t proc_time = get_process_times_u64(ph);
            next_prev_proc_times[pid] = proc_time;

            if (has_prev && delta_wall > 0) {
              auto it = prev_proc_times.find(pid);
              if (it != prev_proc_times.end() && proc_time >= it->second) {
                uint64_t delta_proc = proc_time - it->second;
                // FILETIME units are 100ns; delta_wall is also in 100ns units
                ps.cpu_percent = 100.0 * static_cast<double>(delta_proc) / static_cast<double>(delta_wall);
              }
            }

            ps.mem_rss_bytes = get_process_rss(ph);

            // Try to get command (full image path) — may fail for system/privileged processes
            std::string cmd = query_process_command(ph);
            if (!cmd.empty())
              ps.command = std::move(cmd);
            else
              ps.command = "[" + ps.name + "]";

            CloseHandle(ph);
          } else {
            // Could not open process: fall back to name-only representation.
            ps.mem_rss_bytes = 0;
            ps.command = "[" + ps.name + "]";
          }

          snap.processes.push_back(std::move(ps));
        } while (Process32NextW(snap_h, &pe));
      }
      CloseHandle(snap_h);
    }

    prev_proc_times = std::move(next_prev_proc_times);

    prev_total = cur_total;
    prev_idle = cur_idle;
    prev_wall = cur_wall;
    has_prev = true;

    return snap;
  }
};

process_info_source::process_info_source() : impl_(std::make_unique<impl>()) {}
process_info_source::~process_info_source() = default;

system_snapshot process_info_source::sample() { return impl_->sample(); }

} // namespace bdg::wish
