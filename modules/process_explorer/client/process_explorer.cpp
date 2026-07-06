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
#include "modules/process_explorer/client/process_explorer.hpp"
#include "modules/process_explorer/client/process_info.hpp"

#include "app/wish_cli/client/app_registry.hpp"
#include "app/wish_cli/client/wish_app_host.hpp"

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
    processes[i++] = dynamic_ptr{e};
  }
  args["processes"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(processes))};

  return args;
}

} // namespace

void run_process_explorer(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "ProcessExplorer"_key).get());
  auto stop = std::make_shared<std::atomic<bool>>(false);

  proxy->onEvent("closed"_key, [&s, stop](dynamic) {
    stop->store(true, std::memory_order_relaxed);
    s.signal_done();
  });

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
        .description = "top/htop-style system monitor; client samples CPU/memory/processes, server only renders",
        .params = {},
        .run = run_process_explorer,
    });
  }
};
const process_explorer_app_registrar process_explorer_app_registrar_instance;
} // namespace

} // namespace bdg::wish
