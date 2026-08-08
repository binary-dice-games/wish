// MIT License © 2025 Binary Dice Games
/// @file process_explorer.cpp
/// @brief Client-side runner for the Process Explorer embedded app.
///
/// The ProcessExplorer form (server-side) never samples anything itself --
/// it only renders whatever snapshot it was last given. This runner owns
/// the actual process/CPU/memory sampling (inherently local to whichever
/// machine the client is running on) and periodically pushes a fresh
/// snapshot into the form via its `update_snapshot` RMI method, mirroring
/// how Notepad's reference client owns upload_file/download_file while the
/// server only manages tabs (see modules/notepad/client/notepad.cpp).
#include "modules/bdg/desktop/process_explorer/client/process_explorer.hpp"
#include "modules/bdg/desktop/process_explorer/client/process_control.hpp"
#include "modules/bdg/desktop/process_explorer/client/process_info.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace bdg::wish {

using namespace bison;

namespace {

// bison::dynamic fields only support float (not double) among floating-
// point alternatives, so every numeric value on the wire is narrowed here.
dynamic encode_snapshot(const system_snapshot& snap) {
  dynamic args;
  args["cpu_percent"_key] = static_cast<float>(snap.system.cpu_percent);
  args["mem_total_bytes"_key] = static_cast<float>(snap.system.mem_total_bytes);
  args["mem_used_bytes"_key] = static_cast<float>(snap.system.mem_used_bytes);

  std::vector<float> per_core(snap.system.per_core_percent.begin(), snap.system.per_core_percent.end());
  args["per_core_percent"_key] = std::move(per_core);

  dynamic processes;
  size_t i = 0;
  for (auto& p : snap.processes) {
    auto e = std::make_shared<dynamic>();
    (*e)["pid"_key] = static_cast<int32_t>(p.pid);
    (*e)["name"_key] = p.name;
    (*e)["command"_key] = p.command;
    (*e)["state"_key] = std::string(1, p.state);
    (*e)["cpu_percent"_key] = static_cast<float>(p.cpu_percent);
    (*e)["mem_rss_bytes"_key] = static_cast<float>(p.mem_rss_bytes);
    (*e)["nice_value"_key] = static_cast<int32_t>(p.nice);
    std::vector<int32_t> affinity(p.affinity_cores.begin(), p.affinity_cores.end());
    (*e)["affinity_cores"_key] = std::move(affinity);
    processes[i++] = dynamic_ptr{e};
  }
  args["processes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(processes))};

  return args;
}

// Runs the process-control action `payload` describes ({pid, action, and
// action-specific extra fields -- see process_explorer.hpp's doc comment on
// "on_process_action_requested"}) and reports the outcome back to the form
// via `report_action_result`, mirroring how file_explorer's client reports
// upload/download outcomes.
void handle_action_requested(const std::shared_ptr<rmi::proxy::dynamic>& proxy, const dynamic& payload) {
  int pid = payload.as<int32_t>("pid"_key);
  std::string action = payload.as<std::string>("action"_key);

  process_action_result result;
  if (action == "kill") {
    result = kill_process(pid);
  } else if (action == "pause") {
    result = pause_process(pid);
  } else if (action == "resume") {
    result = resume_process(pid);
  } else if (action == "set_priority") {
    result = set_process_priority(pid, payload.as<int32_t>("nice"_key));
  } else if (action == "set_affinity") {
    std::vector<int> cores;
    if (auto* c = payload.findField<std::vector<int32_t>>("cores"_key))
      cores.assign(c->begin(), c->end());
    result = set_process_affinity(pid, cores);
  } else {
    result.success = false;
    result.error = "Unknown action: " + action;
  }

  dynamic report;
  report["pid"_key] = static_cast<int32_t>(pid);
  report["action"_key] = action;
  report["success"_key] = result.success;
  report["error"_key] = result.error;
  try {
    proxy->call("report_action_result"_key, std::move(report)).get();
  } catch (const std::exception&) {
    // Form already torn down (window closed / session ending) -- nothing to report to.
  }
}

// Fetches on-demand extended info for the "Properties" dialog and reports
// it back via `report_process_details`.
void handle_details_requested(const std::shared_ptr<rmi::proxy::dynamic>& proxy, const dynamic& payload) {
  int pid = payload.as<int32_t>("pid"_key);
  process_details d = get_process_details(pid);

  dynamic report;
  report["pid"_key] = static_cast<int32_t>(pid);
  report["found"_key] = d.found;
  if (d.found) {
    report["ppid"_key] = static_cast<int32_t>(d.ppid);
    report["user"_key] = d.user;
    report["thread_count"_key] = static_cast<int32_t>(d.thread_count);
    report["start_time"_key] = d.start_time;
    report["exe_path"_key] = d.exe_path;
    report["cwd"_key] = d.cwd;
    report["cmdline"_key] = d.cmdline;
    report["nice"_key] = static_cast<int32_t>(d.nice);
    std::vector<int32_t> affinity(d.affinity_cores.begin(), d.affinity_cores.end());
    report["affinity_cores"_key] = std::move(affinity);
  } else {
    report["error"_key] = d.error;
  }
  try {
    proxy->call("report_process_details"_key, std::move(report)).get();
  } catch (const std::exception&) {
    // Form already torn down -- nothing to report to.
  }
}

} // namespace

void run_process_explorer(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "ProcessExplorer"_key).get());
  auto stop = std::make_shared<std::atomic<bool>>(false);

  proxy->onEvent("closed"_key, [&s, stop](dynamic) {
    stop->store(true, std::memory_order_relaxed);
    s.signal_done();
  });

  proxy->onEvent("on_process_action_requested"_key, [proxy](dynamic payload) { handle_action_requested(proxy, payload); });
  proxy->onEvent("on_process_details_requested"_key, [proxy](dynamic payload) { handle_details_requested(proxy, payload); });

  // `proxy` and `stop` stay alive via the shared_ptrs captured below (and by
  // the onEvent lambda above) -- no separate keep_alive() call needed,
  // mirroring notepad's `notepad` proxy (see its comment).
  std::thread([proxy, stop] {
    using namespace std::chrono_literals;
    process_info_source source;
    while (!stop->load(std::memory_order_relaxed)) {
      auto snap = source.sample();
      try {
        proxy->call("update_snapshot"_key, encode_snapshot(snap)).get();
      } catch (const std::exception&) {
        // Form already torn down (window closed / session ending) -- stop.
        break;
      }
      for (int i = 0; i < 10 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
    }
  }).detach();
}

namespace {
struct process_explorer_app_registrar {
  process_explorer_app_registrar() {
    register_app({
        .name = "process_explorer",
        .organization = WISH_MODULE_BDG_DESKTOP_PROCESS_EXPLORER_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_PROCESS_EXPLORER_COLLECTION,
        .description = "top/htop-style system monitor; client samples CPU/memory/processes, server only renders",
        .params = {},
        .run = run_process_explorer,
    });
  }
};
const process_explorer_app_registrar process_explorer_app_registrar_instance;
} // namespace

} // namespace bdg::wish
