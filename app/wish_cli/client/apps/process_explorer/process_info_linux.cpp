// MIT License © 2025 Binary Dice Games
/// @file process_info_linux.cpp
/// @brief Linux/MSYS2 implementation of process_info_source (reads /proc).
///
/// This is the only file in this directory that knows about /proc; a future
/// native-Windows port would add a process_info_windows.cpp implementing the
/// same process_info_source interface (see process_info.hpp) without
/// touching any other file here.
#include "process_info.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

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
// are fields 14/15, i.e. tokens[11]/tokens[12].
bool read_proc_pid_stat(int pid, char& state, uint64_t& utime, uint64_t& stime) {
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
  return true;
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

} // namespace

struct process_info_source::impl {
  bool has_prev{false};
  cpu_times overall_prev;
  std::vector<cpu_times> per_core_prev;
  std::unordered_map<int, uint64_t> prev_proc_jiffies;

  system_snapshot sample() {
    system_snapshot snap;

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
      if (!read_proc_pid_stat(pid, state, utime, stime))
        continue; // process exited mid-scan, or unreadable — skip like top does

      process_sample ps;
      ps.pid = pid;
      ps.state = state;
      ps.name = read_proc_pid_comm(pid);
      ps.command = read_proc_pid_cmdline(pid);
      if (ps.command.empty())
        ps.command = "[" + ps.name + "]";
      ps.mem_rss_bytes = read_proc_pid_rss_bytes(pid);

      uint64_t jiffies = utime + stime;
      next_prev_proc_jiffies[pid] = jiffies;
      if (has_prev && delta_total > 0) {
        auto it = prev_proc_jiffies.find(pid);
        if (it != prev_proc_jiffies.end() && jiffies >= it->second) {
          uint64_t delta_proc = jiffies - it->second;
          ps.cpu_percent = 100.0 * static_cast<double>(delta_proc) / static_cast<double>(delta_total);
        }
      }

      snap.processes.push_back(std::move(ps));
    }
    prev_proc_jiffies = std::move(next_prev_proc_jiffies);

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
