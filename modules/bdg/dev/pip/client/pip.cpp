// MIT License © 2026 Binary Dice Games
/// @file pip.cpp
/// @brief Client-side runner for the pip (PipFrontend) embedded app.
///
/// `wish client --run=pip [-- <python-or-venv>]` -- the optional positional
/// argument picks the Python environment to manage: an interpreter path /
/// name, or a virtualenv directory. Without it the `python3` (else `python`)
/// on `PATH` is used, which is an activated virtualenv's when there is one.
/// Owns a pip_source instance (all actual `pip` invocation + parsing) and
/// wires the PipFrontend form's `*_requested` events to it -- see
/// server/pip.hpp for the full event contract.
#include "pip.hpp"
#include "pip_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <iostream>
#include <memory>

namespace bdg::wish {

using namespace bison;

void run_pip(wish_app_host& s) {
  const std::string arg = s.app_args().empty() ? std::string{} : s.app_args()[0];
  const std::string interpreter = pip::resolve_interpreter(arg);
  if (interpreter.empty()) {
    std::cerr << "pip: no Python interpreter found in '" << arg << "' (expected a virtualenv directory)\n";
    s.signal_done();
    return;
  }

  // Fast-fail if pip can't be run rather than opening empty windows. The
  // source has no proxy yet: probe_version() doesn't need one.
  std::string error;
  const std::string version = pip::pip_source{nullptr, interpreter, nullptr}.probe_version(error);
  if (version.empty()) {
    std::cerr << "pip: cannot run `" << interpreter << " -m pip`" << (error.empty() ? std::string{} : (": " + error))
              << "\n";
    s.signal_done();
    return;
  }

  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "PipFrontend"_key).get());
  // Every handler below runs as a job on this worker's thread: running pip
  // inside an event handler would block the whole UI until it exits. Long
  // commands get a modal progress dialog (common/command_worker.hpp).
  auto worker = std::make_shared<dev::command_worker>(s, "Running pip");
  worker->start();
  auto source = std::make_shared<pip::pip_source>(proxy, interpreter, worker);

  // Optional payload string (absent -> "").
  auto str = [](const dynamic& payload, bison::key_t key) {
    auto* f = payload.findField<std::string>(key);
    return f ? *f : std::string{};
  };
  auto flag = [](const dynamic& payload, bison::key_t key) {
    auto* f = payload.findField<bool>(key);
    return f && *f;
  };
  auto options = [flag](const dynamic& payload) {
    pip::pip_source::install_options opts;
    opts.upgrade = flag(payload, "upgrade"_key);
    opts.user = flag(payload, "user"_key);
    opts.pre = flag(payload, "pre"_key);
    return opts;
  };

  worker->on(*proxy, "refresh_requested"_key, [source](dynamic) { source->refresh_all(); });
  worker->on(*proxy, "outdated_requested"_key, [source](dynamic) { source->on_outdated_requested(); });

  worker->on(*proxy, "install_requested"_key, [source, str, options](dynamic payload) {
    source->on_install_requested(str(payload, "spec"_key), options(payload));
  });
  worker->on(*proxy, "requirements_requested"_key, [source, str, options](dynamic payload) {
    source->on_requirements_requested(str(payload, "path"_key), options(payload));
  });
  worker->on(*proxy, "package_action_requested"_key, [source, str](dynamic payload) {
    source->on_package_action(str(payload, "name"_key), str(payload, "action"_key));
  });
  worker->on(*proxy, "versions_requested"_key, [source, str, flag](dynamic payload) {
    source->on_versions_requested(str(payload, "name"_key), flag(payload, "pre"_key));
  });
  worker->on(*proxy, "details_requested"_key, [source, str](dynamic payload) {
    source->on_details_requested(str(payload, "kind"_key), str(payload, "name"_key));
  });

  proxy->onEvent("closed"_key, [&s, worker](dynamic) {
    worker->shutdown();
    s.signal_done();
  });

  // Initial population -- queued here, now that every handler is registered,
  // rather than via a form-emitted event that would race ahead of this
  // wiring (the docker / git / kubectl initial-load-race fix).
  source->push_environment(version);
  worker->post([source] { source->refresh_all(); });
}

namespace {
struct pip_app_registrar {
  pip_app_registrar() {
    register_app({
        .name = "pip",
        .organization = WISH_MODULE_BDG_DEV_PIP_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_PIP_COLLECTION,
        .description = "GUI frontend for the local `pip` CLI: list, install, upgrade and uninstall Python "
                       "packages (wish client --run=pip [-- <python-or-venv>])",
        .params = {{"python", "Python interpreter or virtualenv directory to manage (optional; default: the "
                              "python3 on PATH)"}},
        .run = run_pip,
    });
  }
};
const pip_app_registrar pip_app_registrar_instance;
} // namespace

} // namespace bdg::wish
