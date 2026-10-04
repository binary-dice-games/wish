// MIT License © 2026 Binary Dice Games
/// @file docker_source.cpp
/// @brief Implementation of docker_source.
#include "docker_source.hpp"

#include <chrono>
#include <sstream>
#include <thread>

namespace bdg::wish::docker {

using namespace bdg::bison;
using dev::split;
using dev::trim_eol;

namespace {

// "12.34%" -> 12.34f ; "--" / "" / unparseable -> 0. std::stof stops at the
// trailing '%' on its own.
float parse_percent(const std::string& s) {
  try {
    return std::stof(s);
  } catch (const std::exception&) {
    return 0.0f;
  }
}

} // namespace

docker_source::docker_source(
    std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker)
    : tool_source(std::move(proxy), std::move(worker), "docker") {}

docker_source::~docker_source() {
  stop_follow();
  stop_stats_polling();
}

void docker_source::stop_follow() {
  if (follow_stop_)
    follow_stop_->store(true, std::memory_order_relaxed);
  follow_stop_.reset();
}

void docker_source::stop_stats_polling() {
  if (stats_stop_)
    stats_stop_->store(true, std::memory_order_relaxed);
  stats_stop_.reset();
}

void docker_source::start_stats_polling() {
  if (stats_stop_)
    return; // already polling

  auto stop = std::make_shared<std::atomic<bool>>(false);
  stats_stop_ = stop;
  auto proxy = proxy_;
  std::thread([proxy, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      // `docker stats --no-stream` samples for ~1 s internally; the
      // tab-delimited Go template mirrors push_rows()'s parsing shape.
      // run_process() directly, NOT run_logged() -- a ~3 s re-poll would
      // flood the Console window (git's Log-window lesson).
      auto r = dev::run_process(
          {"docker", "stats", "--no-stream", "--no-trunc", "--format",
           "{{.ID}}\t{{.Name}}\t{{.CPUPerc}}\t{{.MemPerc}}\t{{.MemUsage}}"});

      dynamic args;
      dynamic arr;
      size_t n = 0;
      if (r.ok()) {
        std::istringstream iss(r.stdout_text);
        std::string line;
        while (std::getline(iss, line)) {
          line = trim_eol(line);
          if (line.empty())
            continue;
          auto cols = split(line, '\t');
          cols.resize(5);
          auto e = std::make_shared<dynamic>();
          (*e)["id"_key] = cols[0];
          (*e)["name"_key] = cols[1].empty() ? cols[0].substr(0, 12) : cols[1];
          (*e)["cpu_percent"_key] = parse_percent(cols[2]);
          (*e)["mem_percent"_key] = parse_percent(cols[3]);
          (*e)["mem_usage"_key] = cols[4];
          arr[n++] = dynamic_ptr{e};
        }
      } else {
        args["error"_key] = trim_eol(dev::error_output(r));
      }
      args["entries"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
      try {
        proxy->call("update_stats"_key, std::move(args)).get();
      } catch (const std::exception&) {
        break; // form torn down.
      }

      for (int k = 0; k < 30 && !stop->load(std::memory_order_relaxed); ++k)
        std::this_thread::sleep_for(100ms);
    }
  }).detach();
}

void docker_source::refresh_all() {
  push_containers();
  push_images();
  push_volumes();
  push_networks();
}

// ── snapshots (tab-delimited `--format` templates, git_repo_source shape) ───

void docker_source::push_containers() {
  push_rows(
      {"ps", "-a", "--no-trunc", "--format",
       "{{.ID}}\t{{.Names}}\t{{.Image}}\t{{.State}}\t{{.Status}}\t{{.Ports}}\t{{.RunningFor}}"},
      7, "containers"_key, "update_containers"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["id"_key] = c[0];
        e["name"_key] = c[1];
        e["image"_key] = c[2];
        e["state"_key] = c[3];
        e["status"_key] = c[4];
        e["ports"_key] = c[5];
        e["created"_key] = c[6];
      });
}

void docker_source::push_images() {
  push_rows(
      {"images", "--format", "{{.ID}}\t{{.Repository}}\t{{.Tag}}\t{{.CreatedSince}}\t{{.Size}}"},
      5, "images"_key, "update_images"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["id"_key] = c[0];
        e["repository"_key] = c[1];
        e["tag"_key] = c[2];
        e["created"_key] = c[3];
        e["size"_key] = c[4];
      });
}

void docker_source::push_volumes() {
  push_rows(
      {"volume", "ls", "--format", "{{.Name}}\t{{.Driver}}\t{{.Mountpoint}}"}, 3, "volumes"_key,
      "update_volumes"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["name"_key] = c[0];
        e["driver"_key] = c[1];
        e["mountpoint"_key] = c[2];
      });
}

void docker_source::push_networks() {
  push_rows(
      {"network", "ls", "--format", "{{.ID}}\t{{.Name}}\t{{.Driver}}\t{{.Scope}}"}, 4,
      "networks"_key, "update_networks"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["id"_key] = c[0];
        e["name"_key] = c[1];
        e["driver"_key] = c[2];
        e["scope"_key] = c[3];
      });
}

// ── mutating actions ───────────────────────────────────────────────────────

void docker_source::run_and_refresh(
    const std::string& label, const std::string& scope, const std::vector<std::string>& args) {
  auto r = run_logged(args);
  if (report(label, scope, r.ok(), dev::error_output(r)))
    refresh_all();
}

void docker_source::on_container_action(const std::string& id, const std::string& action) {
  if (action == "remove")
    run_and_refresh("remove", "containers", {"rm", "-f", id});
  else
    run_and_refresh(action, "containers", {action, id}); // start/stop/restart/pause/unpause/kill
}

void docker_source::on_image_action(const std::string& id, const std::string& action) {
  if (action == "remove")
    run_and_refresh("remove image", "images", {"rmi", id});
  else if (action == "run")
    run_and_refresh("run", "images", {"run", "-d", "--rm", id});
}

void docker_source::on_volume_action(const std::string& name, const std::string& action) {
  if (action == "remove")
    run_and_refresh("remove volume", "volumes", {"volume", "rm", name});
}

void docker_source::on_network_action(const std::string& id, const std::string& action) {
  if (action == "remove")
    run_and_refresh("remove network", "networks", {"network", "rm", id});
}

void docker_source::on_prune(const std::string& scope) {
  if (scope == "containers")
    run_and_refresh("prune containers", "containers", {"container", "prune", "-f"});
  else if (scope == "images")
    run_and_refresh("prune images", "images", {"image", "prune", "-f"});
  else if (scope == "volumes")
    run_and_refresh("prune volumes", "volumes", {"volume", "prune", "-f"});
  else if (scope == "networks")
    run_and_refresh("prune networks", "networks", {"network", "prune", "-f"});
}

void docker_source::on_pull_image(const std::string& ref) {
  run_and_refresh("pull " + ref, "images", {"pull", ref});
}

void docker_source::on_create_volume(const std::string& name) {
  run_and_refresh("create volume", "volumes", {"volume", "create", name});
}

// ── logs / inspect ─────────────────────────────────────────────────────────

void docker_source::push_logs_snapshot(const std::string& id, int32_t lines) {
  const std::string tail = std::to_string(lines > 0 ? lines : 500);
  auto r = run_logged({"logs", "--tail", tail, "--timestamps", id});
  std::string text = r.ok() ? r.stdout_text + r.stderr_text // docker logs writes app stderr too
                            : dev::error_output(r);

  dynamic args;
  args["container_id"_key] = id;
  args["title"_key] = "logs: " + id.substr(0, 12);
  args["text"_key] = std::move(text);
  call("update_logs"_key, std::move(args));
}

void docker_source::on_logs_requested(const std::string& id, bool follow, int32_t lines) {
  stop_follow(); // any prior follow thread is now stale (different container / unfollow).
  if (id.empty())
    return;

  push_logs_snapshot(id, lines);

  if (!follow)
    return;

  auto stop = std::make_shared<std::atomic<bool>>(false);
  follow_stop_ = stop;
  auto proxy = proxy_;
  std::thread([proxy, id, lines, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      for (int i = 0; i < 20 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
      if (stop->load(std::memory_order_relaxed))
        break;
      // Deliberately run_process(), not run_logged() -- a ~2 s re-poll
      // would flood the Console window (git's Log-window lesson).
      auto r = dev::run_process({"docker", "logs", "--tail", std::to_string(lines > 0 ? lines : 500), "--timestamps", id});
      dynamic args;
      args["container_id"_key] = id;
      args["title"_key] = "logs: " + id.substr(0, 12) + "  (following)";
      args["text"_key] = r.ok() ? r.stdout_text + r.stderr_text
                                : dev::error_output(r);
      try {
        proxy->call("update_logs"_key, std::move(args)).get();
      } catch (const std::exception&) {
        break; // form torn down.
      }
    }
  }).detach();
}

void docker_source::on_inspect_requested(const std::string& kind, const std::string& id) {
  if (id.empty())
    return;
  // `docker container|image|volume|network inspect <id>` -- the bare
  // `docker inspect` also works for containers/images but the scoped form
  // is unambiguous.
  std::vector<std::string> argv;
  if (kind == "container" || kind == "image" || kind == "volume" || kind == "network")
    argv = {kind, "inspect", id};
  else
    argv = {"inspect", id};
  auto r = run_logged(argv);

  dynamic args;
  args["target_id"_key] = id;
  args["kind"_key] = kind;
  args["title"_key] = kind + ": " + id.substr(0, 19);
  args["text"_key] = r.ok() ? r.stdout_text : dev::error_output(r);
  call("update_inspect"_key, std::move(args));
}

} // namespace bdg::wish::docker
