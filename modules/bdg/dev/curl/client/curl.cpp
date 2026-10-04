// MIT License © 2026 Binary Dice Games
/// @file curl.cpp
/// @brief Client-side runner for the curl (CurlFrontend) embedded app.
///
/// `wish client --run=curl` -- no positional args; the module shells out
/// to whatever `curl` binary is on PATH. Owns a curl_source instance (all
/// actual `curl` invocation, response parsing, and local persistence) and
/// wires the CurlFrontend form's `*_requested` events to it -- see
/// server/curl.hpp for the full event contract.
#include "curl.hpp"
#include "curl_source.hpp"

#include "modules/bdg/dev/common/frontend.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

namespace bdg::wish {

using namespace bison;

void run_curl(wish_app_host& s) {
  // Fast-fail if there is no `curl` binary on PATH at all, rather than
  // opening an empty window -- mirrors docker's `docker version` gate,
  // adapted: curl has no daemon to reach, just a binary to find.
  auto check = dev::run_process({"curl", "--version"});
  if (!check.ok()) {
    dev::fail_startup(s, "curl: `curl` binary not found on PATH", check.stderr_text);
    return;
  }

  // Every handler below runs as a job on the frontend's worker thread:
  // running the tool inside an event handler would block the whole UI until
  // it exits. Long commands get a modal progress dialog
  // (modules/bdg/common/command_worker.hpp).
  const auto frontend = dev::open_frontend(s, "CurlFrontend"_key, "Sending request");
  const auto& proxy = frontend.proxy;
  const auto& worker = frontend.worker;
  auto source = std::make_shared<curl::curl_source>(proxy, s, worker);

  worker->on(
      *proxy, "send_requested"_key, [source](dynamic payload) { source->on_send_requested(payload); });
  worker->on(
      *proxy, "save_request_requested"_key, [source](dynamic payload) { source->on_save_request_requested(payload); });
  worker->on(*proxy, "load_request_requested"_key, [source](dynamic payload) {
    source->on_load_request_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "delete_request_requested"_key, [source](dynamic payload) {
    source->on_delete_request_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "duplicate_request_requested"_key, [source](dynamic payload) {
    source->on_duplicate_request_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "load_history_requested"_key, [source](dynamic payload) {
    source->on_load_history_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "clear_history_requested"_key, [source](dynamic) { source->on_clear_history_requested(); });
  worker->on(*proxy, "new_environment_requested"_key, [source](dynamic payload) {
    source->on_new_environment_requested(payload.as<std::string>("name"_key));
  });
  worker->on(*proxy, "delete_environment_requested"_key, [source](dynamic payload) {
    source->on_delete_environment_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "select_environment_requested"_key, [source](dynamic payload) {
    source->on_select_environment_requested(payload.as<std::string>("id"_key));
  });
  worker->on(*proxy, "save_environment_vars_requested"_key, [source](dynamic payload) {
    source->on_save_environment_vars_requested(payload);
  });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (docker/git's documented initial-load-race
  // fix).
  worker->post([source] { source->refresh_all(); });
}

namespace {
struct curl_app_registrar {
  curl_app_registrar() {
    register_app({
        .name = "curl",
        .organization = WISH_MODULE_BDG_DEV_CURL_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_CURL_COLLECTION,
        .description = "Postman-style GUI frontend for the local `curl` binary (wish client --run=curl)",
        .params = {},
        .run = run_curl,
    });
  }
};
const curl_app_registrar curl_app_registrar_instance;
} // namespace

} // namespace bdg::wish
