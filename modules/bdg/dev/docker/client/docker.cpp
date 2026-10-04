// MIT License © 2026 Binary Dice Games
/// @file docker.cpp
/// @brief Client-side runner for the docker (DockerFrontend) embedded app.
///
/// `wish client --run=docker` -- no positional args; the module talks to
/// whatever Docker daemon the `docker` CLI itself would (DOCKER_HOST /
/// default socket / current context). Owns a docker_source instance (all
/// actual `docker` invocation + parsing) and wires the DockerFrontend
/// form's `*_requested` events to it -- see server/docker.hpp for the full
/// event contract.
#include "docker.hpp"
#include "docker_source.hpp"

#include "modules/bdg/dev/common/frontend.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

namespace bdg::wish {

using namespace bison;

void run_docker(wish_app_host& s) {
  // Fast-fail if the daemon isn't reachable (docker not on PATH, socket
  // permission, daemon down) rather than opening an empty window -- mirrors
  // git's `rev-parse --is-inside-work-tree` gate. `--format
  // '{{.Server.Version}}'` also confirms the *daemon* answered, not just
  // that the client binary exists.
  auto check = dev::run_process({"docker", "version", "--format", "{{.Server.Version}}"});
  if (!check.ok()) {
    dev::fail_startup(s, "docker: cannot reach the Docker daemon", check.stderr_text);
    return;
  }

  // Every handler below runs as a job on the frontend's worker thread:
  // running the tool inside an event handler would block the whole UI until
  // it exits. Long commands get a modal progress dialog
  // (modules/bdg/common/command_worker.hpp).
  const auto frontend = dev::open_frontend(s, "DockerFrontend"_key, "Running docker");
  const auto& proxy = frontend.proxy;
  const auto& worker = frontend.worker;
  auto source = std::make_shared<docker::docker_source>(proxy, worker);

  worker->on(*proxy, "refresh_requested"_key, [source](dynamic) { source->refresh_all(); });

  worker->on(*proxy, "container_action_requested"_key, [source](dynamic payload) {
    source->on_container_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "image_action_requested"_key, [source](dynamic payload) {
    source->on_image_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "volume_action_requested"_key, [source](dynamic payload) {
    source->on_volume_action(payload.as<std::string>("name"_key), payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "network_action_requested"_key, [source](dynamic payload) {
    source->on_network_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  worker->on(
      *proxy, "prune_requested"_key, [source](dynamic payload) { source->on_prune(payload.as<std::string>("scope"_key)); });
  worker->on(
      *proxy, "pull_image_requested"_key, [source](dynamic payload) { source->on_pull_image(payload.as<std::string>("ref"_key)); });
  worker->on(*proxy, "create_volume_requested"_key, [source](dynamic payload) {
    source->on_create_volume(payload.as<std::string>("name"_key));
  });
  worker->on(*proxy, "logs_requested"_key, [source](dynamic payload) {
    source->on_logs_requested(
        payload.as<std::string>("id"_key), payload.as<bool>("follow"_key), payload.as<int32_t>("lines"_key));
  });
  worker->on(*proxy, "inspect_requested"_key, [source](dynamic payload) {
    source->on_inspect_requested(payload.as<std::string>("kind"_key), payload.as<std::string>("id"_key));
  });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (git's documented initial-load-race fix).
  worker->post([source] { source->refresh_all(); });

  // Live `docker stats` graphs in the Stats window: a background poll thread
  // (3 s cadence) that never touches the Console window.
  source->start_stats_polling();
}

namespace {
struct docker_app_registrar {
  docker_app_registrar() {
    register_app({
        .name = "docker",
        .organization = WISH_MODULE_BDG_DEV_DOCKER_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_DOCKER_COLLECTION,
        .description = "Docker Desktop-style GUI frontend for the local `docker` CLI (wish client --run=docker)",
        .params = {},
        .run = run_docker,
    });
  }
};
const docker_app_registrar docker_app_registrar_instance;
} // namespace

} // namespace bdg::wish
