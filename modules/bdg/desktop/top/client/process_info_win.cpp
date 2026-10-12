// MIT License © 2025 Binary Dice Games
/// @file process_info_win.cpp
/// @brief Windows implementation of process_info_source.
///
/// Implements the same sampling interface as process_info_linux.cpp but
/// using Win32 APIs (GetSystemTimes, Toolhelp32Snapshot, GetProcessTimes,
/// GetProcessMemoryInfo, GlobalMemoryStatusEx, QueryFullProcessImageName,
/// GetIfTable2, IOCTL_DISK_PERFORMANCE, GetExtendedTcpTable/UdpTable).
///
/// iphlpapi and ws2_32 come in through uv_a, which every module client
/// target links (see cmake/WishModules.cmake) and which itself requires them.
#include "process_info.hpp"

// winsock2.h must precede windows.h (which otherwise pulls in winsock 1).
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>
#include <winioctl.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

// Maps a Win32 priority class back to the same discrete nice-value scale
// process_control_win.cpp's set_process_priority() maps *forward* from
// (19/10/0/-5/-10/-20) -- kept in sync with that function's buckets so a
// process shows the correct checked entry in the Priority submenu without
// the two files having to share a literal table.
int priority_class_to_nice(DWORD priority_class) {
  switch (priority_class) {
    case IDLE_PRIORITY_CLASS:
      return 19;
    case BELOW_NORMAL_PRIORITY_CLASS:
      return 10;
    case ABOVE_NORMAL_PRIORITY_CLASS:
      return -5;
    case HIGH_PRIORITY_CLASS:
      return -10;
    case REALTIME_PRIORITY_CLASS:
      return -20;
    case NORMAL_PRIORITY_CLASS:
    default:
      return 0;
  }
}

std::vector<int> get_process_affinity_cores(HANDLE h) {
  std::vector<int> cores;
  DWORD_PTR process_mask = 0, system_mask = 0;
  if (!GetProcessAffinityMask(h, &process_mask, &system_mask))
    return cores;
  for (int i = 0; i < static_cast<int>(sizeof(DWORD_PTR) * 8); ++i)
    if (process_mask & (static_cast<DWORD_PTR>(1) << i))
      cores.push_back(i);
  return cores;
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

// ── Network / disk counters ──────────────────────────────────────────────────

struct io_counters {
  uint64_t first{0}; ///< Received (network) or read (disk) bytes.
  uint64_t second{0}; ///< Sent (network) or written (disk) bytes.
};

// Sums received/sent bytes over physical network adapters. Loopback and
// filter interfaces (e.g. the WFP/QoS layers stacked on a Wi-Fi adapter,
// which repeat its traffic) are skipped, as are purely software ones.
io_counters read_network_counters() {
  io_counters total;
  MIB_IF_TABLE2* table = nullptr;
  if (GetIfTable2(&table) != NO_ERROR || !table)
    return total;
  for (ULONG i = 0; i < table->NumEntries; ++i) {
    const MIB_IF_ROW2& row = table->Table[i];
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || !row.InterfaceAndOperStatusFlags.HardwareInterface ||
        row.InterfaceAndOperStatusFlags.FilterInterface)
      continue;
    total.first += row.InOctets;
    total.second += row.OutOctets;
  }
  FreeMibTable(table);
  return total;
}

// Sums read/written bytes over \\.\PhysicalDriveN. Opening with no access
// rights is enough for IOCTL_DISK_PERFORMANCE and needs no elevation.
io_counters read_disk_counters() {
  io_counters total;
  constexpr int kMaxPhysicalDrives = 32;
  for (int i = 0; i < kMaxPhysicalDrives; ++i) {
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      continue;
    DISK_PERFORMANCE perf{};
    DWORD returned = 0;
    if (DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0, &perf, sizeof(perf), &returned, nullptr)) {
      total.first += static_cast<uint64_t>(perf.BytesRead.QuadPart);
      total.second += static_cast<uint64_t>(perf.BytesWritten.QuadPart);
    }
    CloseHandle(h);
  }
  return total;
}

// ── Listening ports ──────────────────────────────────────────────────────────

std::string address_to_string(int family, const void* addr) {
  char buf[INET6_ADDRSTRLEN] = {};
  if (!inet_ntop(family, addr, buf, sizeof(buf)))
    return {};
  return buf;
}

// Calls a GetExtended{Tcp,Udp}Table-style @p fetch(buffer, &size) until the
// buffer is large enough (the table can grow between the two calls).
template <typename Fetch>
std::vector<char> fetch_table(Fetch fetch) {
  std::vector<char> buffer;
  DWORD size = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD rc = fetch(buffer.empty() ? nullptr : buffer.data(), &size);
    if (rc == NO_ERROR)
      return buffer;
    if (rc != ERROR_INSUFFICIENT_BUFFER)
      break;
    buffer.resize(size);
  }
  return {};
}

// TCP sockets in the LISTEN state and every bound UDP socket, IPv4 and IPv6,
// with their owning pid. The process name is filled in by the caller.
std::vector<open_port> read_listening_ports() {
  std::vector<open_port> ports;
  auto add = [&](const char* protocol, std::string address, DWORD port_field, DWORD pid) {
    open_port p;
    p.protocol = protocol;
    p.address = std::move(address);
    p.port = ntohs(static_cast<u_short>(port_field));
    p.pid = static_cast<int>(pid);
    ports.push_back(std::move(p));
  };

  auto tcp4 = fetch_table([](void* buf, DWORD* size) {
    return GetExtendedTcpTable(buf, size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
  });
  if (!tcp4.empty()) {
    auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(tcp4.data());
    for (DWORD i = 0; i < t->dwNumEntries; ++i)
      add("TCP",
          address_to_string(AF_INET, &t->table[i].dwLocalAddr),
          t->table[i].dwLocalPort,
          t->table[i].dwOwningPid);
  }
  auto tcp6 = fetch_table([](void* buf, DWORD* size) {
    return GetExtendedTcpTable(buf, size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_LISTENER, 0);
  });
  if (!tcp6.empty()) {
    auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(tcp6.data());
    for (DWORD i = 0; i < t->dwNumEntries; ++i)
      add("TCP6",
          address_to_string(AF_INET6, t->table[i].ucLocalAddr),
          t->table[i].dwLocalPort,
          t->table[i].dwOwningPid);
  }
  auto udp4 = fetch_table(
      [](void* buf, DWORD* size) { return GetExtendedUdpTable(buf, size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0); });
  if (!udp4.empty()) {
    auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(udp4.data());
    for (DWORD i = 0; i < t->dwNumEntries; ++i)
      add("UDP",
          address_to_string(AF_INET, &t->table[i].dwLocalAddr),
          t->table[i].dwLocalPort,
          t->table[i].dwOwningPid);
  }
  auto udp6 = fetch_table(
      [](void* buf, DWORD* size) { return GetExtendedUdpTable(buf, size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0); });
  if (!udp6.empty()) {
    auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(udp6.data());
    for (DWORD i = 0; i < t->dwNumEntries; ++i)
      add("UDP6",
          address_to_string(AF_INET6, t->table[i].ucLocalAddr),
          t->table[i].dwLocalPort,
          t->table[i].dwOwningPid);
  }
  return ports;
}

} // namespace

struct process_info_source::impl {
  bool has_prev{false};
  uint64_t prev_total{0};
  uint64_t prev_idle{0};
  uint64_t prev_wall{0};
  std::unordered_map<int, uint64_t> prev_proc_times;
  std::chrono::steady_clock::time_point prev_time;
  io_counters prev_net;
  io_counters prev_disk;

  system_snapshot sample() {
    system_snapshot snap;

    auto now = std::chrono::steady_clock::now();
    io_counters net = read_network_counters();
    io_counters disk = read_disk_counters();
    snap.system.net_rx_total_bytes = net.first;
    snap.system.net_tx_total_bytes = net.second;
    snap.system.disk_read_total_bytes = disk.first;
    snap.system.disk_write_total_bytes = disk.second;
    if (has_prev) {
      double elapsed = std::chrono::duration<double>(now - prev_time).count();
      snap.system.net_rx_bytes_per_sec = counter_rate(prev_net.first, net.first, elapsed);
      snap.system.net_tx_bytes_per_sec = counter_rate(prev_net.second, net.second, elapsed);
      snap.system.disk_read_bytes_per_sec = counter_rate(prev_disk.first, disk.first, elapsed);
      snap.system.disk_write_bytes_per_sec = counter_rate(prev_disk.second, disk.second, elapsed);
    }
    prev_time = now;
    prev_net = net;
    prev_disk = disk;
    std::unordered_map<int, std::string> pid_names;

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
          pid_names[pid] = ps.name;
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
            ps.nice = priority_class_to_nice(GetPriorityClass(ph));
            ps.affinity_cores = get_process_affinity_cores(ph);

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

    snap.ports = read_listening_ports();
    for (auto& p : snap.ports) {
      auto it = pid_names.find(p.pid);
      if (it != pid_names.end())
        p.process = it->second;
    }
    sort_ports(snap.ports);

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
