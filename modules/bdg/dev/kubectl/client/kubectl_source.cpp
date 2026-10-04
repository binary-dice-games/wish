// MIT License © 2026 Binary Dice Games
/// @file kubectl_source.cpp
/// @brief Implementation of kubectl_source.
#include "kubectl_source.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <thread>

namespace bdg::wish::kubectl {

using namespace bdg::bison;
using dev::split;
using dev::trim_eol;

namespace {

// `kubectl top` CPU column -> millicores. "5m" -> 5 ; "1" -> 1000 (whole
// cores) ; "1500000n" -> 1.5 ; "250000u" -> 0.25.
float parse_cpu_millicores(const std::string& s) {
  if (s.empty())
    return 0.0f;
  try {
    size_t idx = 0;
    double v = std::stod(s, &idx);
    const std::string unit = s.substr(idx);
    if (unit == "n")
      return static_cast<float>(v / 1e6);
    if (unit == "u")
      return static_cast<float>(v / 1e3);
    if (unit == "m")
      return static_cast<float>(v);
    return static_cast<float>(v * 1000.0);
  } catch (const std::exception&) {
    return 0.0f;
  }
}

// `kubectl top` memory column -> MiB. Handles Ki/Mi/Gi/Ti, K/M/G (SI), and
// bare bytes.
float parse_mem_mib(const std::string& s) {
  if (s.empty())
    return 0.0f;
  try {
    size_t idx = 0;
    double v = std::stod(s, &idx);
    const std::string unit = s.substr(idx);
    if (unit == "Ki")
      return static_cast<float>(v / 1024.0);
    if (unit == "Mi")
      return static_cast<float>(v);
    if (unit == "Gi")
      return static_cast<float>(v * 1024.0);
    if (unit == "Ti")
      return static_cast<float>(v * 1024.0 * 1024.0);
    if (unit == "K" || unit == "k")
      return static_cast<float>(v * 1000.0 / 1048576.0);
    if (unit == "M")
      return static_cast<float>(v * 1e6 / 1048576.0);
    if (unit == "G")
      return static_cast<float>(v * 1e9 / 1048576.0);
    return static_cast<float>(v / 1048576.0);
  } catch (const std::exception&) {
    return 0.0f;
  }
}

// "30%" -> 30.0f ; "<unknown>" / "" -> 0. std::stof stops at the '%'.
float parse_pct(const std::string& s) {
  try {
    return std::stof(s);
  } catch (const std::exception&) {
    return 0.0f;
  }
}

std::string first_token(const std::string& s) {
  auto ws = s.find_first_not_of(" \t");
  if (ws == std::string::npos)
    return {};
  auto we = s.find_first_of(" \t", ws);
  return s.substr(ws, we == std::string::npos ? std::string::npos : we - ws);
}

// "true false true" -> "2/3"; "" -> "0/0".
std::string ready_ratio(const std::string& bools) {
  auto toks = split(bools, ' ');
  int total = 0, up = 0;
  for (auto& t : toks) {
    if (t.empty())
      continue;
    ++total;
    if (t == "true")
      ++up;
  }
  return std::to_string(up) + "/" + std::to_string(total);
}

// "0 2 0" -> "2"; "" -> "0".
std::string restart_sum(const std::string& counts) {
  long sum = 0;
  for (auto& t : split(counts, ' ')) {
    if (t.empty())
      continue;
    sum += std::strtol(t.c_str(), nullptr, 10);
  }
  return std::to_string(sum);
}

std::string or_zero(const std::string& s) {
  return s.empty() ? std::string{"0"} : s;
}

// "80 443" -> "80, 443".
std::string join_ports(const std::string& raw) {
  std::string out;
  for (auto& t : split(raw, ' ')) {
    if (t.empty())
      continue;
    if (!out.empty())
      out += ", ";
    out += t;
  }
  return out;
}

std::time_t parse_rfc3339_utc(const std::string& s) {
  int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6)
    return 0;
  std::tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon = M - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min = m;
  tm.tm_sec = sec;
#if defined(_WIN32)
  return _mkgmtime(&tm);
#else
  return timegm(&tm);
#endif
}

// RFC3339 timestamp -> short "3h" / "2d" style age (kubectl's own format).
std::string humanize_age(const std::string& ts) {
  std::time_t then = parse_rfc3339_utc(ts);
  if (then == 0)
    return {};
  long d = static_cast<long>(std::time(nullptr) - then);
  if (d < 0)
    d = 0;
  if (d < 60)
    return std::to_string(d) + "s";
  if (d < 3600)
    return std::to_string(d / 60) + "m";
  if (d < 86400)
    return std::to_string(d / 3600) + "h";
  if (d < 86400L * 365)
    return std::to_string(d / 86400) + "d";
  return std::to_string(d / (86400L * 365)) + "y";
}

} // namespace

kubectl_source::kubectl_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker)
    : tool_source(std::move(proxy), std::move(worker), "kubectl") {}

kubectl_source::~kubectl_source() {
  stop_follow();
  stop_stats_polling();
}

void kubectl_source::stop_follow() {
  if (follow_stop_)
    follow_stop_->store(true, std::memory_order_relaxed);
  follow_stop_.reset();
}

void kubectl_source::stop_stats_polling() {
  if (stats_stop_)
    stats_stop_->store(true, std::memory_order_relaxed);
  stats_stop_.reset();
}

void kubectl_source::start_stats_polling() {
  if (stats_stop_)
    return; // already polling

  auto stop = std::make_shared<std::atomic<bool>>(false);
  stats_stop_ = stop;
  auto proxy = proxy_;
  std::thread([proxy, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      // run_process() directly, NOT run_logged() -- a ~10 s re-poll of
      // two commands would flood the Console window (git's Log-window lesson).
      auto pods_r = dev::run_process({"kubectl", "top", "pods", "-A", "--no-headers"});
      auto nodes_r = dev::run_process({"kubectl", "top", "nodes", "--no-headers"});

      dynamic args;
      dynamic pods_arr;
      dynamic nodes_arr;
      size_t pi = 0;
      size_t ni = 0;
      std::string error;

      if (pods_r.ok()) {
        std::istringstream iss(pods_r.stdout_text);
        std::string line;
        while (std::getline(iss, line)) {
          auto c = dev::words(line);
          if (c.size() < 4)
            continue;
          auto e = std::make_shared<dynamic>();
          (*e)["namespace"_key] = c[0];
          (*e)["name"_key] = c[1];
          (*e)["cpu"_key] = c[2];
          (*e)["cpu_m"_key] = parse_cpu_millicores(c[2]);
          (*e)["mem"_key] = c[3];
          (*e)["mem_mib"_key] = parse_mem_mib(c[3]);
          pods_arr[pi++] = dynamic_ptr{e};
        }
      } else {
        error = trim_eol(dev::error_output(pods_r));
      }

      if (nodes_r.ok()) {
        std::istringstream iss(nodes_r.stdout_text);
        std::string line;
        while (std::getline(iss, line)) {
          auto c = dev::words(line);
          if (c.size() < 5)
            continue;
          auto e = std::make_shared<dynamic>();
          (*e)["name"_key] = c[0];
          (*e)["cpu"_key] = c[1];
          (*e)["cpu_m"_key] = parse_cpu_millicores(c[1]);
          (*e)["cpu_pct"_key] = parse_pct(c[2]);
          (*e)["mem"_key] = c[3];
          (*e)["mem_mib"_key] = parse_mem_mib(c[3]);
          (*e)["mem_pct"_key] = parse_pct(c[4]);
          nodes_arr[ni++] = dynamic_ptr{e};
        }
      } else if (error.empty()) {
        error = trim_eol(dev::error_output(nodes_r));
      }

      args["pods"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(pods_arr))};
      args["nodes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(nodes_arr))};
      if (!error.empty())
        args["error"_key] = error;

      try {
        proxy->call("update_stats"_key, std::move(args)).get();
      } catch (const std::exception&) {
        break; // form torn down.
      }

      for (int k = 0; k < 100 && !stop->load(std::memory_order_relaxed); ++k)
        std::this_thread::sleep_for(100ms);
    }
  }).detach();
}

void kubectl_source::refresh_all() {
  push_pods();
  push_deployments();
  push_services();
  push_nodes();
}

// ── snapshots (tab-delimited `-o jsonpath` templates, docker_source shape) ──

void kubectl_source::push_pods() {
  push_rows(
      {"get", "pods", "-A", "-o",
       "jsonpath={range .items[*]}"
       "{.metadata.namespace}{\"\\t\"}{.metadata.name}{\"\\t\"}{.status.phase}{\"\\t\"}"
       "{.status.containerStatuses[*].ready}{\"\\t\"}{.status.containerStatuses[*].restartCount}{\"\\t\"}"
       "{.status.containerStatuses[*].state.waiting.reason}{\"\\t\"}{.metadata.creationTimestamp}{\"\\n\"}{end}"},
      7, "pods"_key, "update_pods"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["namespace"_key] = c[0];
        e["name"_key] = c[1];
        e["phase"_key] = c[2];
        e["ready"_key] = ready_ratio(c[3]);
        e["restarts"_key] = restart_sum(c[4]);
        e["reason"_key] = first_token(c[5]);
        e["age"_key] = humanize_age(c[6]);
      });
}

void kubectl_source::push_deployments() {
  push_rows(
      {"get", "deployments", "-A", "-o",
       "jsonpath={range .items[*]}"
       "{.metadata.namespace}{\"\\t\"}{.metadata.name}{\"\\t\"}{.status.readyReplicas}{\"\\t\"}{.spec.replicas}{\"\\t\"}"
       "{.status.updatedReplicas}{\"\\t\"}{.status.availableReplicas}{\"\\t\"}{.metadata.creationTimestamp}{\"\\n\"}{end}"},
      7, "deployments"_key, "update_deployments"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["namespace"_key] = c[0];
        e["name"_key] = c[1];
        e["ready"_key] = or_zero(c[2]) + "/" + or_zero(c[3]);
        e["uptodate"_key] = or_zero(c[4]);
        e["available"_key] = or_zero(c[5]);
        e["age"_key] = humanize_age(c[6]);
      });
}

void kubectl_source::push_services() {
  push_rows(
      {"get", "services", "-A", "-o",
       "jsonpath={range .items[*]}"
       "{.metadata.namespace}{\"\\t\"}{.metadata.name}{\"\\t\"}{.spec.type}{\"\\t\"}{.spec.clusterIP}{\"\\t\"}"
       "{.spec.ports[*].port}{\"\\t\"}{.metadata.creationTimestamp}{\"\\n\"}{end}"},
      6, "services"_key, "update_services"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["namespace"_key] = c[0];
        e["name"_key] = c[1];
        e["type"_key] = c[2];
        e["cluster_ip"_key] = c[3];
        e["ports"_key] = join_ports(c[4]);
        e["age"_key] = humanize_age(c[5]);
      });
}

void kubectl_source::push_nodes() {
  push_rows(
      {"get", "nodes", "-o",
       "jsonpath={range .items[*]}"
       "{.metadata.name}{\"\\t\"}{.status.conditions[?(@.type==\"Ready\")].status}{\"\\t\"}{.spec.unschedulable}{\"\\t\"}"
       "{.status.nodeInfo.kubeletVersion}{\"\\t\"}{.metadata.creationTimestamp}{\"\\n\"}{end}"},
      5, "nodes"_key, "update_nodes"_key, [](dynamic& e, const std::vector<std::string>& c) {
        const bool cordoned = c[2] == "true";
        std::string status = c[1] == "True" ? "Ready" : "NotReady";
        if (cordoned)
          status += ",SchedulingDisabled";
        e["name"_key] = c[0];
        e["status"_key] = status;
        e["schedulable"_key] = cordoned ? std::string{"false"} : std::string{"true"};
        e["version"_key] = c[3];
        e["age"_key] = humanize_age(c[4]);
      });
}

// ── mutating actions ───────────────────────────────────────────────────────

void kubectl_source::run_and_refresh(
    const std::string& label, const std::string& scope, const std::vector<std::string>& args) {
  auto r = run_logged(args);
  if (report(label, scope, r.ok(), dev::error_output(r)))
    refresh_all();
}

void kubectl_source::on_pod_action(const std::string& name, const std::string& ns, const std::string& action) {
  if (action == "delete")
    run_and_refresh("delete pod " + name, "pods", {"delete", "pod", name, "-n", ns});
}

void kubectl_source::on_deployment_action(
    const std::string& name, const std::string& ns, const std::string& action) {
  if (action == "restart")
    run_and_refresh("rollout restart " + name, "deployments", {"rollout", "restart", "deployment", name, "-n", ns});
  else if (action == "delete")
    run_and_refresh("delete deployment " + name, "deployments", {"delete", "deployment", name, "-n", ns});
}

void kubectl_source::on_service_action(
    const std::string& name, const std::string& ns, const std::string& action) {
  if (action == "delete")
    run_and_refresh("delete service " + name, "services", {"delete", "service", name, "-n", ns});
}

void kubectl_source::on_node_action(const std::string& name, const std::string& action) {
  if (action == "cordon")
    run_and_refresh("cordon " + name, "nodes", {"cordon", name});
  else if (action == "uncordon")
    run_and_refresh("uncordon " + name, "nodes", {"uncordon", name});
  else if (action == "drain")
    run_and_refresh(
        "drain " + name, "nodes",
        {"drain", name, "--ignore-daemonsets", "--delete-emptydir-data", "--force"});
}

// ── logs / describe ────────────────────────────────────────────────────────

void kubectl_source::push_logs_snapshot(
    const std::string& name, const std::string& ns, int32_t lines, bool following) {
  const std::string tail = std::to_string(lines > 0 ? lines : 500);
  auto r = run_logged({"logs", name, "-n", ns, "--tail", tail, "--timestamps"});
  std::string text = r.ok() ? r.stdout_text : dev::error_output(r);

  dynamic args;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["title"_key] = "logs: " + ns + "/" + name + (following ? "  (following)" : "");
  args["text"_key] = std::move(text);
  call("update_logs"_key, std::move(args));
}

void kubectl_source::on_logs_requested(
    const std::string& name, const std::string& ns, bool follow, int32_t lines) {
  stop_follow(); // any prior follow thread is now stale (different pod / unfollow).
  if (name.empty())
    return;

  push_logs_snapshot(name, ns, lines, follow);

  if (!follow)
    return;

  auto stop = std::make_shared<std::atomic<bool>>(false);
  follow_stop_ = stop;
  auto proxy = proxy_;
  std::thread([proxy, name, ns, lines, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      for (int i = 0; i < 20 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
      if (stop->load(std::memory_order_relaxed))
        break;
      // Deliberately run_process(), not run_logged() -- a ~2 s re-poll
      // would flood the Console window (git's Log-window lesson).
      auto r = dev::run_process(
          {"kubectl", "logs", name, "-n", ns, "--tail", std::to_string(lines > 0 ? lines : 500), "--timestamps"});
      dynamic args;
      args["name"_key] = name;
      args["namespace"_key] = ns;
      args["title"_key] = "logs: " + ns + "/" + name + "  (following)";
      args["text"_key] = r.ok() ? r.stdout_text : dev::error_output(r);
      try {
        proxy->call("update_logs"_key, std::move(args)).get();
      } catch (const std::exception&) {
        break; // form torn down.
      }
    }
  }).detach();
}

void kubectl_source::on_describe_requested(
    const std::string& kind, const std::string& name, const std::string& ns) {
  if (name.empty())
    return;
  std::vector<std::string> argv;
  const std::string k = (kind == "pod" || kind == "deployment" || kind == "service" || kind == "node") ? kind : "pod";
  if (k == "node")
    argv = {"describe", "node", name};
  else
    argv = {"describe", k, name, "-n", ns};
  auto r = run_logged(argv);

  dynamic args;
  args["kind"_key] = kind;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["title"_key] = kind + ": " + (ns.empty() ? name : ns + "/" + name);
  args["text"_key] = r.ok() ? r.stdout_text : dev::error_output(r);
  call("update_describe"_key, std::move(args));
}

} // namespace bdg::wish::kubectl
