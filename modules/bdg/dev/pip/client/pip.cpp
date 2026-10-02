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
  const std::string version = pip::pip_source{nullptr, interpreter}.probe_version(error);
  if (version.empty()) {
    std::cerr << "pip: cannot run `" << interpreter << " -m pip`" << (error.empty() ? std::string{} : (": " + error))
              << "\n";
    s.signal_done();
    return;
  }

  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "PipFrontend"_key).get());
  auto source = std::make_shared<pip::pip_source>(proxy, interpreter);

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

  // Every handler only queues work for the source's worker thread: running
  // pip here would block the UI for as long as the command takes. `src` is
  // safe to capture raw -- the worker thread keeps the source alive.
  source->start();
  auto* src = source.get();

  proxy->onEvent("refresh_requested"_key, [src](dynamic) { src->post([src] { src->refresh_all(); }); });
  proxy->onEvent("outdated_requested"_key, [src](dynamic) { src->post([src] { src->on_outdated_requested(); }); });

  proxy->onEvent("install_requested"_key, [src, str, options](dynamic payload) {
    src->post([src, spec = str(payload, "spec"_key), opts = options(payload)] {
      src->on_install_requested(spec, opts);
    });
  });
  proxy->onEvent("requirements_requested"_key, [src, str, options](dynamic payload) {
    src->post([src, path = str(payload, "path"_key), opts = options(payload)] {
      src->on_requirements_requested(path, opts);
    });
  });
  proxy->onEvent("package_action_requested"_key, [src, str](dynamic payload) {
    src->post([src, name = str(payload, "name"_key), action = str(payload, "action"_key)] {
      src->on_package_action(name, action);
    });
  });
  proxy->onEvent("versions_requested"_key, [src, str, flag](dynamic payload) {
    src->post([src, name = str(payload, "name"_key), pre = flag(payload, "pre"_key)] {
      src->on_versions_requested(name, pre);
    });
  });
  proxy->onEvent("details_requested"_key, [src, str](dynamic payload) {
    src->post([src, kind = str(payload, "kind"_key), name = str(payload, "name"_key)] {
      src->on_details_requested(kind, name);
    });
  });
  proxy->onEvent("cancel_requested"_key, [src](dynamic) { src->cancel(); });

  proxy->onEvent("closed"_key, [&s, src](dynamic) {
    src->shutdown();
    s.signal_done();
  });

  // Initial population -- queued here, now that every onEvent() handler is
  // registered, rather than via a form-emitted event that would race ahead
  // of this wiring (the docker / git / kubectl initial-load-race fix).
  source->push_environment(version);
  src->post([src] { src->refresh_all(); });
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
