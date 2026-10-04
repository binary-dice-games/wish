// MIT License © 2026 Binary Dice Games
/// @file frontend.hpp
/// @brief Startup wiring shared by the bdg/dev modules' client entry points
///        (`run_<tool>(wish_app_host&)`).
///
/// Every dev module's client opens its server-side `<Tool>Frontend` form,
/// starts one command_worker that runs the form's `*_requested` events off
/// the UI thread, and shuts both down when the form closes. open_frontend()
/// does exactly that; the payload_*() getters read the optional fields of an
/// event payload. Header-only.
#pragma once

#include "modules/bdg/dev/common/command_worker.hpp"
#include "modules/bdg/dev/common/process.hpp"

#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"
#include "src/rmi/client/proxy.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace bdg::wish::dev {

/// @brief A dev module's open form and the worker that runs its commands.
struct frontend {
  std::shared_ptr<bison::rmi::proxy::dynamic> proxy;
  std::shared_ptr<command_worker> worker;
};

/// @brief Instantiates the `wish` form class @p form, starts a
/// command_worker whose progress dialog is titled @p progress_title (e.g.
/// `"Running helm"`), and wires the form's `closed` event to shut the worker
/// down and end the app.
///
/// Register every `*_requested` handler (command_worker::on()) before
/// posting the initial population job: a form-emitted "load" event would
/// race ahead of that wiring.
inline frontend open_frontend(wish_app_host& host, bison::key_t form, const std::string& progress_title) {
  frontend f;
  f.proxy = std::make_shared<bison::rmi::proxy::dynamic>(host.instantiate(bison::key_t{"wish"}, form).get());
  f.worker = std::make_shared<command_worker>(host, progress_title);
  f.worker->start();
  f.proxy->onEvent(bison::key_t{"closed"}, [&host, worker = f.worker](bison::dynamic) {
    worker->shutdown();
    host.signal_done();
  });
  return f;
}

/// @brief Ends the app before any form opened: prints `<message>[:
/// <detail>]` to stderr and signals the host. For a tool that is not
/// installed / not reachable, so the module fails fast instead of opening
/// empty windows.
inline void fail_startup(wish_app_host& host, const std::string& message, const std::string& detail = {}) {
  std::cerr << message << (detail.empty() ? std::string{} : ": " + detail) << "\n";
  host.signal_done();
}

/// @brief String field @p key of an event payload; `""` when absent.
inline std::string payload_string(const bison::dynamic& payload, bison::key_t key) {
  auto* f = payload.findField<std::string>(key);
  return f ? *f : std::string{};
}

/// @brief Boolean field @p key of an event payload; false when absent.
inline bool payload_flag(const bison::dynamic& payload, bison::key_t key) {
  auto* f = payload.findField<bool>(key);
  return f && *f;
}

/// @brief Integer field @p key of an event payload; @p fallback when absent.
inline int32_t payload_int(const bison::dynamic& payload, bison::key_t key, int32_t fallback = 0) {
  auto* f = payload.findField<int32_t>(key);
  return f ? *f : fallback;
}

} // namespace bdg::wish::dev
