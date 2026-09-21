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
#include "curl_process.hpp"
#include "curl_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <iostream>
#include <memory>

namespace bdg::wish {

using namespace bison;

void run_curl(wish_app_host& s) {
  // Fast-fail if there is no `curl` binary on PATH at all, rather than
  // opening an empty window -- mirrors docker's `docker version` gate,
  // adapted: curl has no daemon to reach, just a binary to find.
  auto check = curl::run_curl_cli({"--version"});
  if (!check.ok()) {
    std::cerr << "curl: `curl` binary not found on PATH"
              << (check.stderr_text.empty() ? std::string{} : (": " + check.stderr_text)) << "\n";
    s.signal_done();
    return;
  }

  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "CurlFrontend"_key).get());
  auto source = std::make_shared<curl::curl_source>(proxy, s);

  proxy->onEvent(
      "send_requested"_key, [source](dynamic payload) { source->on_send_requested(payload); });
  proxy->onEvent(
      "save_request_requested"_key, [source](dynamic payload) { source->on_save_request_requested(payload); });
  proxy->onEvent("load_request_requested"_key, [source](dynamic payload) {
    source->on_load_request_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("delete_request_requested"_key, [source](dynamic payload) {
    source->on_delete_request_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("duplicate_request_requested"_key, [source](dynamic payload) {
    source->on_duplicate_request_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("load_history_requested"_key, [source](dynamic payload) {
    source->on_load_history_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("clear_history_requested"_key, [source](dynamic) { source->on_clear_history_requested(); });
  proxy->onEvent("new_environment_requested"_key, [source](dynamic payload) {
    source->on_new_environment_requested(payload.as<std::string>("name"_key));
  });
  proxy->onEvent("delete_environment_requested"_key, [source](dynamic payload) {
    source->on_delete_environment_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("select_environment_requested"_key, [source](dynamic payload) {
    source->on_select_environment_requested(payload.as<std::string>("id"_key));
  });
  proxy->onEvent("save_environment_vars_requested"_key, [source](dynamic payload) {
    source->on_save_environment_vars_requested(payload);
  });

  proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Initial population -- called directly here, now that every onEvent()
  // handler is registered, rather than via a form-emitted event that would
  // race ahead of this wiring (docker/git's documented initial-load-race
  // fix).
  source->refresh_all();
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
