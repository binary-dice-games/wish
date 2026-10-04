// MIT License © 2026 Binary Dice Games
/// @file kubectl.cpp
/// @brief Client-side runner for the kubectl (KubectlFrontend) embedded app.
///
/// `wish client --run=kubectl` -- no positional args; the module talks to
/// whatever cluster the `kubectl` CLI itself would (the current
/// kubeconfig / `KUBECONFIG` / current-context). Owns a kubectl_source
/// instance (all actual `kubectl` invocation + parsing) and wires the
/// KubectlFrontend form's `*_requested` events to it -- see
/// server/kubectl.hpp for the full event contract.
#include "kubectl.hpp"
#include "kubectl_source.hpp"

#include "modules/bdg/dev/common/frontend.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

namespace bdg::wish {

using namespace bison;

void run_kubectl(wish_app_host& s) {
  // Fast-fail if the cluster isn't reachable (kubectl not on PATH, no
  // context, API server down) rather than opening empty windows -- mirrors
  // docker's `docker version` gate. `version -o json` contacts the API
  // server for the server version, so a non-zero exit here means the
  // cluster, not just the client binary, is unavailable.
  auto check = dev::run_process({"kubectl", "version", "-o", "json"});
  if (!check.ok()) {
    dev::fail_startup(s, "kubectl: cannot reach the cluster", check.stderr_text);
    return;
  }

  // Every handler below runs as a job on the frontend's worker thread:
  // running the tool inside an event handler would block the whole UI until
  // it exits. Long commands get a modal progress dialog
  // (modules/bdg/common/command_worker.hpp).
  const auto frontend = dev::open_frontend(s, "KubectlFrontend"_key, "Running kubectl");
  const auto& proxy = frontend.proxy;
  const auto& worker = frontend.worker;
  auto source = std::make_shared<kubectl::kubectl_source>(proxy, worker);

  worker->on(*proxy, "refresh_requested"_key, [source](dynamic) { source->refresh_all(); });

  worker->on(*proxy, "pod_action_requested"_key, [source](dynamic payload) {
    source->on_pod_action(
        payload.as<std::string>("name"_key), payload.as<std::string>("namespace"_key),
        payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "deployment_action_requested"_key, [source](dynamic payload) {
    source->on_deployment_action(
        payload.as<std::string>("name"_key), payload.as<std::string>("namespace"_key),
        payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "service_action_requested"_key, [source](dynamic payload) {
    source->on_service_action(
        payload.as<std::string>("name"_key), payload.as<std::string>("namespace"_key),
        payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "node_action_requested"_key, [source](dynamic payload) {
    source->on_node_action(payload.as<std::string>("name"_key), payload.as<std::string>("action"_key));
  });
  worker->on(*proxy, "logs_requested"_key, [source](dynamic payload) {
    source->on_logs_requested(
        payload.as<std::string>("name"_key), payload.as<std::string>("namespace"_key),
        payload.as<bool>("follow"_key), payload.as<int32_t>("lines"_key));
  });
  worker->on(*proxy, "describe_requested"_key, [source](dynamic payload) {
    source->on_describe_requested(
        payload.as<std::string>("kind"_key), payload.as<std::string>("name"_key),
        payload.as<std::string>("namespace"_key));
  });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (docker's / git's documented initial-load-race
  // fix).
  worker->post([source] { source->refresh_all(); });

  // Live `kubectl top` graphs in the Top window: a background poll thread
  // (10 s cadence) that never touches the Console window.
  source->start_stats_polling();
}

namespace {
struct kubectl_app_registrar {
  kubectl_app_registrar() {
    register_app({
        .name = "kubectl",
        .organization = WISH_MODULE_BDG_DEV_KUBECTL_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_KUBECTL_COLLECTION,
        .description =
            "Kubernetes-dashboard-style GUI frontend for the local `kubectl` CLI (wish client --run=kubectl)",
        .params = {},
        .run = run_kubectl,
    });
  }
};
const kubectl_app_registrar kubectl_app_registrar_instance;
} // namespace

} // namespace bdg::wish
