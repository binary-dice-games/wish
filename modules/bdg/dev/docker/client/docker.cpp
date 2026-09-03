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
#include "docker_process.hpp"
#include "docker_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <iostream>
#include <memory>

namespace bdg::wish {

using namespace bison;

void run_docker(wish_app_host& s) {
  // Fast-fail if the daemon isn't reachable (docker not on PATH, socket
  // permission, daemon down) rather than opening an empty window -- mirrors
  // git's `rev-parse --is-inside-work-tree` gate. `--format
  // '{{.Server.Version}}'` also confirms the *daemon* answered, not just
  // that the client binary exists.
  auto check = docker::run_docker_cli({"version", "--format", "{{.Server.Version}}"});
  if (!check.ok()) {
    std::cerr << "docker: cannot reach the Docker daemon"
              << (check.stderr_text.empty() ? std::string{} : (": " + check.stderr_text)) << "\n";
    s.signal_done();
    return;
  }

  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "DockerFrontend"_key).get());
  auto source = std::make_shared<docker::docker_source>(proxy);

  proxy->onEvent("refresh_requested"_key, [source](dynamic) { source->refresh_all(); });

  proxy->onEvent("container_action_requested"_key, [source](dynamic payload) {
    source->on_container_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  proxy->onEvent("image_action_requested"_key, [source](dynamic payload) {
    source->on_image_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  proxy->onEvent("volume_action_requested"_key, [source](dynamic payload) {
    source->on_volume_action(payload.as<std::string>("name"_key), payload.as<std::string>("action"_key));
  });
  proxy->onEvent("network_action_requested"_key, [source](dynamic payload) {
    source->on_network_action(payload.as<std::string>("id"_key), payload.as<std::string>("action"_key));
  });
  proxy->onEvent(
      "prune_requested"_key, [source](dynamic payload) { source->on_prune(payload.as<std::string>("scope"_key)); });
  proxy->onEvent(
      "pull_image_requested"_key, [source](dynamic payload) { source->on_pull_image(payload.as<std::string>("ref"_key)); });
  proxy->onEvent("create_volume_requested"_key, [source](dynamic payload) {
    source->on_create_volume(payload.as<std::string>("name"_key));
  });
  proxy->onEvent("logs_requested"_key, [source](dynamic payload) {
    source->on_logs_requested(
        payload.as<std::string>("id"_key), payload.as<bool>("follow"_key), payload.as<int32_t>("lines"_key));
  });
  proxy->onEvent("inspect_requested"_key, [source](dynamic payload) {
    source->on_inspect_requested(payload.as<std::string>("kind"_key), payload.as<std::string>("id"_key));
  });

  proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (git's documented initial-load-race fix).
  source->refresh_all();
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
