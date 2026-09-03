// MIT License © 2026 Binary Dice Games
/// @file kubectl_source.cpp
/// @brief Implementation of kubectl_source.
#include "kubectl_source.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <sstream>
#include <thread>

namespace bdg::wish::kubectl {

using namespace bdg::bison;

namespace {

// Split `s` on `sep`, keeping empty fields.
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::string trim_eol(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return s;
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

// Runs one `kubectl get <kind> -A -o jsonpath=<tmpl>`, calls @p fill once per
// output line with the tab-split fields (padded to @p ncols), then calls
// @p rmi_method with the collected array under @p array_key. Mirrors
// docker_source's push_list().
void push_list(
    const std::shared_ptr<bison::rmi::proxy::dynamic>& proxy, const std::vector<std::string>& argv, size_t ncols,
    key_t array_key, key_t rmi_method, const std::function<void(dynamic&, const std::vector<std::string>&)>& fill) {
  auto r = run_kubectl_cli(argv);

  dynamic arr;
  size_t i = 0;
  if (r.ok()) {
    std::istringstream iss(r.stdout_text);
    std::string line;
    while (std::getline(iss, line)) {
      line = trim_eol(line);
      if (line.empty())
        continue;
      auto cols = split(line, '\t');
      cols.resize(ncols);
      auto e = std::make_shared<dynamic>();
      fill(*e, cols);
      arr[i++] = dynamic_ptr{e};
    }
  }

  dynamic args;
  args[array_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  try {
    proxy->call(rmi_method, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

} // namespace

kubectl_source::kubectl_source(std::shared_ptr<bison::rmi::proxy::dynamic> proxy) : proxy_(std::move(proxy)) {}

kubectl_source::~kubectl_source() {
  stop_follow();
}

void kubectl_source::stop_follow() {
  if (follow_stop_)
    follow_stop_->store(true, std::memory_order_relaxed);
  follow_stop_.reset();
}

void kubectl_source::refresh_all() {
  push_pods();
  push_deployments();
  push_services();
  push_nodes();
}

// ── snapshots (tab-delimited `-o jsonpath` templates, docker_source shape) ──

void kubectl_source::push_pods() {
  push_list(
      proxy_,
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
  push_list(
      proxy_,
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
  push_list(
      proxy_,
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
  push_list(
      proxy_,
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
  auto r = run_kubectl_cli(args);

  dynamic report;
  report["command"_key] = label;
  report["scope"_key] = scope;
  report["ok"_key] = r.ok();
  report["output"_key] = r.ok() ? std::string{} : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  try {
    proxy_->call("command_result"_key, std::move(report)).get();
  } catch (const std::exception&) {
    return; // form gone.
  }
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
  auto r = run_kubectl_cli({"logs", name, "-n", ns, "--tail", tail, "--timestamps"});
  std::string text = r.ok() ? r.stdout_text : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);

  dynamic args;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["title"_key] = "logs: " + ns + "/" + name + (following ? "  (following)" : "");
  args["text"_key] = std::move(text);
  try {
    proxy_->call("update_logs"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
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
      auto r = run_kubectl_cli(
          {"logs", name, "-n", ns, "--tail", std::to_string(lines > 0 ? lines : 500), "--timestamps"});
      dynamic args;
      args["name"_key] = name;
      args["namespace"_key] = ns;
      args["title"_key] = "logs: " + ns + "/" + name + "  (following)";
      args["text"_key] = r.ok() ? r.stdout_text : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
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
  auto r = run_kubectl_cli(argv);

  dynamic args;
  args["kind"_key] = kind;
  args["name"_key] = name;
  args["namespace"_key] = ns;
  args["title"_key] = kind + ": " + (ns.empty() ? name : ns + "/" + name);
  args["text"_key] = r.ok() ? r.stdout_text : (r.stderr_text.empty() ? r.stdout_text : r.stderr_text);
  try {
    proxy_->call("update_describe"_key, std::move(args)).get();
  } catch (const std::exception&) {
  }
}

} // namespace bdg::wish::kubectl
