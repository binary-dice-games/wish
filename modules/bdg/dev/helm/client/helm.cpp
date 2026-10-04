// MIT License © 2026 Binary Dice Games
/// @file helm.cpp
/// @brief Client-side runner for the helm (HelmFrontend) embedded app.
///
/// `wish client --run=helm` -- no positional args; the module manages
/// whatever the `helm` CLI itself would (the current kubeconfig /
/// `KUBECONFIG` / current-context, and the user's own chart repository
/// configuration). Owns a helm_source instance (all actual `helm` invocation
/// + parsing) and wires the HelmFrontend form's `*_requested` events to it --
/// see server/helm.hpp for the full event contract.
#include "helm.hpp"
#include "helm_source.hpp"

#include "modules/bdg/common/frontend.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

namespace bdg::wish {

using namespace bison;

void run_helm(wish_app_host& s) {
  // Fast-fail if `helm` isn't installed rather than opening empty windows.
  // Unlike kubectl's `version` gate this does NOT contact the cluster (helm 3
  // has no server component): the Repositories and Charts windows are useful
  // without one, and an unreachable cluster is reported in the Releases
  // window's status line instead.
  auto check = common::run_process({"helm", "version", "--short"});
  if (!check.ok()) {
    common::fail_startup(s, "helm: cannot run the `helm` CLI", check.stderr_text);
    return;
  }

  // Every handler below runs as a job on the frontend's worker thread:
  // running the tool inside an event handler would block the whole UI until
  // it exits. Long commands get a modal progress dialog
  // (modules/bdg/common/command_worker.hpp).
  const auto frontend = common::open_frontend(s, "HelmFrontend"_key, "Running helm");
  const auto& proxy = frontend.proxy;
  const auto& worker = frontend.worker;
  auto source = std::make_shared<helm::helm_source>(proxy, worker);
  auto str = common::payload_string; // optional payload string (absent -> "")

  worker->on(*proxy, "refresh_requested"_key, [source](dynamic) { source->refresh_all(); });

  worker->on(*proxy, "release_action_requested"_key, [source, str](dynamic payload) {
    source->on_release_action(
        str(payload, "name"_key), str(payload, "namespace"_key), str(payload, "action"_key),
        str(payload, "revision"_key));
  });
  worker->on(*proxy, "repo_action_requested"_key, [source, str](dynamic payload) {
    source->on_repo_action(str(payload, "name"_key), str(payload, "action"_key));
  });
  worker->on(*proxy, "repo_add_requested"_key, [source, str](dynamic payload) {
    source->on_repo_add(str(payload, "name"_key), str(payload, "url"_key));
  });
  worker->on(*proxy, "search_requested"_key, [source, str](dynamic payload) {
    source->on_search_requested(str(payload, "query"_key));
  });
  worker->on(*proxy, "install_requested"_key, [source, str](dynamic payload) {
    helm::helm_source::install_request req;
    req.upgrade = str(payload, "mode"_key) == "upgrade";
    req.chart = str(payload, "chart"_key);
    req.version = str(payload, "version"_key);
    req.release = str(payload, "release"_key);
    req.ns = str(payload, "namespace"_key);
    req.values = str(payload, "values"_key);
    req.create_namespace = common::payload_flag(payload, "create_namespace"_key);
    req.wait = common::payload_flag(payload, "wait"_key);
    source->on_install_requested(req);
  });
  worker->on(*proxy, "install_values_requested"_key, [source, str](dynamic payload) {
    source->on_install_values_requested(
        common::payload_int(payload, "token"_key), str(payload, "source"_key), str(payload, "chart"_key),
        str(payload, "version"_key), str(payload, "name"_key), str(payload, "namespace"_key));
  });
  worker->on(*proxy, "history_requested"_key, [source, str](dynamic payload) {
    source->on_history_requested(str(payload, "name"_key), str(payload, "namespace"_key));
  });
  worker->on(*proxy, "details_requested"_key, [source, str](dynamic payload) {
    source->on_details_requested(
        str(payload, "kind"_key), str(payload, "name"_key), str(payload, "namespace"_key),
        str(payload, "version"_key));
  });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (the docker / git / kubectl initial-load-race
  // fix).
  worker->post([source] { source->refresh_all(); });
  // ... and list every chart of the configured repositories, so the Charts
  // window is usable before the first search.
  worker->post([source] { source->on_search_requested({}); });
}

namespace {
struct helm_app_registrar {
  helm_app_registrar() {
    register_app({
        .name = "helm",
        .organization = WISH_MODULE_BDG_DEV_HELM_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_HELM_COLLECTION,
        .description = "GUI frontend for the local `helm` CLI: releases, repositories and charts "
                       "(wish client --run=helm)",
        .params = {},
        .run = run_helm,
    });
  }
};
const helm_app_registrar helm_app_registrar_instance;
} // namespace

} // namespace bdg::wish
