// MIT License © 2025 Binary Dice Games
/// @file process_info_linux.cpp
/// @brief Linux implementation of process_info_source (reads /proc and /sys).
///
/// This is the only file in this directory that knows about /proc; a future
/// native-Windows port would add a process_info_windows.cpp implementing the
/// same process_info_source interface (see process_info.hpp) without
/// touching any other file here.
#include "process_info.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace bdg::wish {

namespace {

namespace fs = std::filesystem;

struct cpu_times {
  uint64_t total{0};
  uint64_t idle{0};
};

// Parses one "cpu"/"cpuN" line from /proc/stat: user nice system idle iowait
// irq softirq steal guest guest_nice. idle = idle + iowait; total = sum of
// all present fields. Returns false if the line has too few fields to parse.
bool parse_cpu_line(const std::string& line, cpu_times& out) {
  std::istringstream iss(line);
  std::string label;
  iss >> label;
  std::vector<uint64_t> vals;
  uint64_t v;
  while (iss >> v)
    vals.push_back(v);
  if (vals.size() < 4)
    return false;
  out.idle = vals[3] + (vals.size() > 4 ? vals[4] : 0);
  out.total = 0;
  for (auto x : vals)
    out.total += x;
  return true;
}

double percent_from_delta(const cpu_times& prev, const cpu_times& cur) {
  uint64_t delta_total = cur.total > prev.total ? cur.total - prev.total : 0;
  uint64_t delta_idle = cur.idle > prev.idle ? cur.idle - prev.idle : 0;
  if (delta_total == 0)
    return 0.0;
  return 100.0 * static_cast<double>(delta_total - delta_idle) / static_cast<double>(delta_total);
}

// Reads /proc/stat's "cpu" (overall) and "cpuN" (per-core) lines, in order.
bool read_proc_stat(cpu_times& overall, std::vector<cpu_times>& per_core) {
  std::ifstream in("/proc/stat");
  if (!in.is_open())
    return false;
  per_core.clear();
  std::string line;
  bool got_overall = false;
  while (std::getline(in, line)) {
    if (line.rfind("cpu", 0) != 0)
      continue;
    // "cpu " (aggregate) has no digit right after "cpu"; "cpuN" does.
    bool is_core_line = line.size() > 3 && std::isdigit(static_cast<unsigned char>(line[3]));
    cpu_times t;
    if (!parse_cpu_line(line, t))
      continue;
    if (is_core_line)
      per_core.push_back(t);
    else {
      overall = t;
      got_overall = true;
    }
  }
  return got_overall;
}

uint64_t parse_meminfo_kb(const std::string& line) {
  // Format: "Label:      12345 kB"
  auto colon = line.find(':');
  if (colon == std::string::npos)
    return 0;
  std::istringstream iss(line.substr(colon + 1));
  uint64_t kb = 0;
  iss >> kb;
  return kb;
}

void read_proc_meminfo(uint64_t& total_bytes, uint64_t& used_bytes) {
  std::ifstream in("/proc/meminfo");
  if (!in.is_open()) {
    total_bytes = used_bytes = 0;
    return;
  }
  uint64_t mem_total = 0, mem_available = 0, mem_free = 0, buffers = 0, cached = 0;
  bool has_available = false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("MemTotal:", 0) == 0)
      mem_total = parse_meminfo_kb(line);
    else if (line.rfind("MemAvailable:", 0) == 0) {
      mem_available = parse_meminfo_kb(line);
      has_available = true;
    } else if (line.rfind("MemFree:", 0) == 0)
      mem_free = parse_meminfo_kb(line);
    else if (line.rfind("Buffers:", 0) == 0)
      buffers = parse_meminfo_kb(line);
    else if (line.rfind("Cached:", 0) == 0 && line.rfind("SwapCached:", 0) != 0)
      cached = parse_meminfo_kb(line);
  }
  uint64_t available = has_available ? mem_available : (mem_free + buffers + cached);
  uint64_t used = mem_total > available ? mem_total - available : 0;
  total_bytes = mem_total * 1024ULL;
  used_bytes = used * 1024ULL;
}

// Reads /proc/<pid>/stat, splitting after the last ')' since comm may
// contain spaces or parentheses. tokens[0] is state (field 3); utime/stime
// are fields 14/15 (tokens[11]/tokens[12]); nice is field 19 (tokens[16]).
bool read_proc_pid_stat(int pid, char& state, uint64_t& utime, uint64_t& stime, int& nice) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  if (!in.is_open())
    return false;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
  auto close_paren = content.rfind(')');
  if (close_paren == std::string::npos || close_paren + 2 >= content.size())
    return false;
  std::istringstream iss(content.substr(close_paren + 2));
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok)
    tokens.push_back(tok);
  if (tokens.size() < 13)
    return false;
  state = tokens[0].empty() ? '?' : tokens[0][0];
  utime = std::stoull(tokens[11]);
  stime = std::stoull(tokens[12]);
  nice = tokens.size() > 16 ? std::stoi(tokens[16]) : 0;
  return true;
}

// sched_getaffinity() reports which logical cores `pid` is currently
// allowed to run on; capped at `num_cores` (the same count per_core_percent
// is sized to) so a stale/oversized kernel CPU_SETSIZE never produces core
// indices the UI has no meter/checkbox for.
std::vector<int> read_affinity_cores(int pid, size_t num_cores) {
  std::vector<int> cores;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(pid, sizeof(set), &set) != 0)
    return cores;
  for (size_t i = 0; i < num_cores && i < CPU_SETSIZE; ++i)
    if (CPU_ISSET(i, &set))
      cores.push_back(static_cast<int>(i));
  return cores;
}

uint64_t read_proc_pid_rss_bytes(int pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/status");
  if (!in.is_open())
    return 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("VmRSS:", 0) == 0)
      return parse_meminfo_kb(line) * 1024ULL;
  }
  return 0;
}

std::string read_proc_pid_comm(int pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
  if (!in.is_open())
    return {};
  std::string name;
  std::getline(in, name);
  return name;
}

std::string read_proc_pid_cmdline(int pid) {
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

// ── Network / disk counters ──────────────────────────────────────────────────

struct io_counters {
  uint64_t first{0}; ///< Received (network) or read (disk) bytes.
  uint64_t second{0}; ///< Sent (network) or written (disk) bytes.
};

// Sums received/sent bytes over every interface in /proc/net/dev except
// loopback. Each data line is "  name: rx_bytes rx_packets ... (8 rx
// fields) tx_bytes ...", so tx_bytes is the 9th number after the colon.
io_counters read_proc_net_dev() {
  io_counters total;
  std::ifstream in("/proc/net/dev");
  std::string line;
  while (std::getline(in, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos)
      continue; // the two header lines
    std::string name = line.substr(0, colon);
    name.erase(0, name.find_first_not_of(' '));
    if (name == "lo")
      continue;
    std::istringstream iss(line.substr(colon + 1));
    uint64_t v[9] = {};
    for (auto& x : v)
      iss >> x;
    if (!iss)
      continue;
    total.first += v[0];
    total.second += v[8];
  }
  return total;
}

// Sums read/written bytes over every *physical* disk in /proc/diskstats:
// one with a /sys/block/<name>/device link. That skips partitions (not in
// /sys/block at all) and loop/ram/zram/dm/md devices (no device link),
// whose I/O is already counted on the disk underneath them. Sectors are
// always 512 bytes here, whatever the disk's real sector size.
io_counters read_proc_diskstats() {
  io_counters total;
  std::ifstream in("/proc/diskstats");
  std::string line;
  std::error_code ec;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    unsigned major = 0, minor = 0;
    std::string name;
    uint64_t reads = 0, reads_merged = 0, sectors_read = 0, ms_reading = 0;
    uint64_t writes = 0, writes_merged = 0, sectors_written = 0;
    iss >> major >> minor >> name >> reads >> reads_merged >> sectors_read >> ms_reading >> writes >> writes_merged >>
        sectors_written;
    if (!iss || !fs::exists("/sys/block/" + name + "/device", ec))
      continue;
    total.first += sectors_read * 512ULL;
    total.second += sectors_written * 512ULL;
  }
  return total;
}

// ── Listening ports ──────────────────────────────────────────────────────────

constexpr int kTcpListen = 0x0A; ///< TCP_LISTEN in include/net/tcp_states.h.

// Decodes /proc/net/{tcp,udp}{,6}'s "ADDR:PORT" hex form. ADDR is the
// in-memory address printed as one (IPv4) or four (IPv6) 32-bit words with
// %08X, so each word is parsed and copied back byte-for-byte; PORT is
// already in host order.
bool parse_hex_endpoint(const std::string& text, bool v6, std::string& address, int& port) {
  auto colon = text.find(':');
  size_t words = v6 ? 4 : 1;
  if (colon != words * 8)
    return false;
  uint32_t raw[4] = {};
  for (size_t i = 0; i < words; ++i)
    raw[i] = static_cast<uint32_t>(std::stoul(text.substr(i * 8, 8), nullptr, 16));
  port = static_cast<int>(std::stoul(text.substr(colon + 1), nullptr, 16));
  char buf[INET6_ADDRSTRLEN] = {};
  if (v6) {
    in6_addr a{};
    std::memcpy(&a, raw, sizeof(a));
    inet_ntop(AF_INET6, &a, buf, sizeof(buf));
  } else {
    in_addr a{};
    std::memcpy(&a, raw, sizeof(a));
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
  }
  address = buf;
  return true;
}

// Appends every listening socket in one /proc/net table to @p out, and the
// socket's inode to @p inode_of (parallel to @p out) so the owning process
// can be found afterwards. TCP sockets count only in the LISTEN state; UDP
// ones only when they have no fixed remote peer (remote port 0).
void read_proc_net_table(
    const char* path,
    const char* protocol,
    bool v6,
    bool tcp,
    std::vector<open_port>& out,
    std::vector<uint64_t>& inode_of) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line); // header
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string slot, local, remote, state_hex, queues, timer, retransmits, uid, timeout;
    uint64_t inode = 0;
    iss >> slot >> local >> remote >> state_hex >> queues >> timer >> retransmits >> uid >> timeout >> inode;
    if (!iss)
      continue;
    try {
      open_port p;
      std::string remote_address;
      int remote_port = 0;
      if (!parse_hex_endpoint(local, v6, p.address, p.port) ||
          !parse_hex_endpoint(remote, v6, remote_address, remote_port))
        continue;
      int state = static_cast<int>(std::stoul(state_hex, nullptr, 16));
      if (tcp ? state != kTcpListen : remote_port != 0)
        continue;
      p.protocol = protocol;
      out.push_back(std::move(p));
      inode_of.push_back(inode);
    } catch (const std::exception&) {
      // Malformed line -- skip it.
    }
  }
}

// Inodes of every socket @p pid has open, from its /proc/<pid>/fd/* links
// ("socket:[12345]"). Only readable for the caller's own processes unless
// running as root -- other sockets keep pid 0 ("unknown"), like `ss -p`.
void collect_socket_inodes(
    int pid,
    const std::unordered_set<uint64_t>& wanted,
    std::unordered_map<uint64_t, int>& owner) {
  std::error_code ec;
  for (auto& fd : fs::directory_iterator("/proc/" + std::to_string(pid) + "/fd", ec)) {
    char target[64];
    ssize_t n = readlink(fd.path().c_str(), target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = '\0';
    if (std::strncmp(target, "socket:[", 8) != 0)
      continue;
    uint64_t inode = std::strtoull(target + 8, nullptr, 10);
    if (wanted.count(inode))
      owner.emplace(inode, pid);
  }
}

} // namespace

struct process_info_source::impl {
  bool has_prev{false};
  cpu_times overall_prev;
  std::vector<cpu_times> per_core_prev;
  std::unordered_map<int, uint64_t> prev_proc_jiffies;
  std::chrono::steady_clock::time_point prev_time;
  io_counters prev_net;
  io_counters prev_disk;

  system_snapshot sample() {
    system_snapshot snap;

    auto now = std::chrono::steady_clock::now();
    io_counters net = read_proc_net_dev();
    io_counters disk = read_proc_diskstats();
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

    std::vector<uint64_t> port_inodes;
    read_proc_net_table("/proc/net/tcp", "TCP", false, true, snap.ports, port_inodes);
    read_proc_net_table("/proc/net/tcp6", "TCP6", true, true, snap.ports, port_inodes);
    read_proc_net_table("/proc/net/udp", "UDP", false, false, snap.ports, port_inodes);
    read_proc_net_table("/proc/net/udp6", "UDP6", true, false, snap.ports, port_inodes);
    std::unordered_set<uint64_t> wanted_inodes(port_inodes.begin(), port_inodes.end());
    wanted_inodes.erase(0);
    std::unordered_map<uint64_t, int> inode_owner;
    std::unordered_map<int, std::string> pid_names;

    cpu_times overall_cur;
    std::vector<cpu_times> per_core_cur;
    bool have_stat = read_proc_stat(overall_cur, per_core_cur);

    if (have_stat && has_prev) {
      snap.system.cpu_percent = percent_from_delta(overall_prev, overall_cur);
      for (size_t i = 0; i < per_core_cur.size(); ++i) {
        double pct = (i < per_core_prev.size()) ? percent_from_delta(per_core_prev[i], per_core_cur[i]) : 0.0;
        snap.system.per_core_percent.push_back(pct);
      }
    } else {
      snap.system.cpu_percent = 0.0;
      snap.system.per_core_percent.assign(per_core_cur.size(), 0.0);
    }

    read_proc_meminfo(snap.system.mem_total_bytes, snap.system.mem_used_bytes);

    uint64_t delta_total = (have_stat && has_prev && overall_cur.total > overall_prev.total)
                                ? overall_cur.total - overall_prev.total
                                : 0;
    // /proc/stat's aggregate "cpu" line sums jiffies across all logical
    // cores, so delta_total is ~num_cores times the actual wall-clock delta.
    // Per-process CPU% must be normalized against wall-clock jiffies (the
    // top/htop convention -- a process fully using one core reads 100%, one
    // using two cores reads 200%), not against the num_cores-times-larger
    // aggregate, which would understate every process by ~num_cores.
    size_t num_cores = per_core_cur.size();
    uint64_t delta_wall = num_cores > 0 ? delta_total / num_cores : delta_total;

    std::unordered_map<int, uint64_t> next_prev_proc_jiffies;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator("/proc", ec)) {
      if (ec)
        break;
      const std::string fname = entry.path().filename().string();
      if (fname.empty() || !std::all_of(fname.begin(), fname.end(), [](unsigned char c) { return std::isdigit(c); }))
        continue;
      int pid = std::stoi(fname);

      char state = '?';
      uint64_t utime = 0, stime = 0;
      int nice = 0;
      if (!read_proc_pid_stat(pid, state, utime, stime, nice))
        continue; // process exited mid-scan, or unreadable — skip like top does

      process_sample ps;
      ps.pid = pid;
      ps.state = state;
      ps.name = read_proc_pid_comm(pid);
      ps.command = read_proc_pid_cmdline(pid);
      if (ps.command.empty())
        ps.command = "[" + ps.name + "]";
      ps.mem_rss_bytes = read_proc_pid_rss_bytes(pid);
      ps.nice = nice;
      ps.affinity_cores = read_affinity_cores(pid, num_cores);
      if (inode_owner.size() < wanted_inodes.size()) {
        size_t owned_before = inode_owner.size();
        collect_socket_inodes(pid, wanted_inodes, inode_owner);
        if (inode_owner.size() != owned_before)
          pid_names[pid] = ps.name;
      }

      uint64_t jiffies = utime + stime;
      next_prev_proc_jiffies[pid] = jiffies;
      if (has_prev && delta_wall > 0) {
        auto it = prev_proc_jiffies.find(pid);
        if (it != prev_proc_jiffies.end() && jiffies >= it->second) {
          uint64_t delta_proc = jiffies - it->second;
          ps.cpu_percent = 100.0 * static_cast<double>(delta_proc) / static_cast<double>(delta_wall);
        }
      }

      snap.processes.push_back(std::move(ps));
    }
    prev_proc_jiffies = std::move(next_prev_proc_jiffies);

    for (size_t i = 0; i < snap.ports.size(); ++i) {
      auto owner = inode_owner.find(port_inodes[i]);
      if (owner == inode_owner.end())
        continue;
      snap.ports[i].pid = owner->second;
      snap.ports[i].process = pid_names[owner->second];
    }
    sort_ports(snap.ports);

    if (have_stat) {
      overall_prev = overall_cur;
      per_core_prev = per_core_cur;
    }
    has_prev = have_stat;

    return snap;
  }
};

process_info_source::process_info_source() : impl_(std::make_unique<impl>()) {}
process_info_source::~process_info_source() = default;

system_snapshot process_info_source::sample() {
  return impl_->sample();
}

} // namespace bdg::wish
