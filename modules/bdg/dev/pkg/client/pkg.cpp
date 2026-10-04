// MIT License © 2026 Binary Dice Games
/// @file pkg.cpp
/// @brief Client-side runner for the pkg (PkgFrontend) embedded app.
///
/// `wish client --run=pkg [-- <manager> [<elevation>]]` -- one interface over
/// the system package managers pkg_backend.hpp knows (apt, dnf, pacman,
/// brew). The first argument picks the manager; without it the first one
/// found on this machine is used. The second picks how commands that change
/// the system get root: `sudo`, `pkexec`, `none`, or `auto` (the default:
/// nothing when already root, else pkexec when installed, else sudo).
///
/// Owns a pkg_source instance (all command invocation + parsing) and wires
/// the PkgFrontend form's `*_requested` events to it -- see server/pkg.hpp
/// for the full event contract.
#include "pkg.hpp"
#include "pkg_backend.hpp"
#include "pkg_source.hpp"

#include "modules/bdg/dev/common/frontend.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>
#include <optional>

namespace bdg::wish {

using namespace bison;

namespace {

const char* const kUsage = "wish client --run=pkg -- <manager> [sudo|pkexec|none]";

std::string first_line(const std::string& text) {
  return text.substr(0, text.find_first_of("\r\n"));
}

// `auto`: root needs nothing; otherwise prefer pkexec, which can ask for the
// password in a dialog of its own -- this tool has no terminal to ask on.
pkg::elevation detect_elevation() {
  auto id = dev::run_process({"id", "-u"});
  if (id.ok() && first_line(id.stdout_text) == "0")
    return pkg::elevation::none;
  if (dev::run_process({"pkexec", "--version"}).ok())
    return pkg::elevation::pkexec;
  return pkg::elevation::sudo;
}

} // namespace

void run_pkg(wish_app_host& s) {
  const auto& args = s.app_args();
  auto fail = [&s](const std::string& message) { dev::fail_startup(s, "pkg: " + message); };

  // ── Which package manager ──
  std::optional<pkg::manager> chosen;
  std::string version;
  if (!args.empty()) {
    chosen = pkg::manager_from_name(args[0]);
    if (!chosen) {
      fail("unknown package manager '" + args[0] + "'. Supported: " + pkg::manager_names() + ".\n     Usage: " +
           kUsage);
      return;
    }
    const auto probe = pkg::probe_command(*chosen);
    auto r = dev::run_process(probe.argv);
    if (!r.ok()) {
      // exit_code -1: the program could not even be started.
      const std::string why = r.exit_code == -1
          ? "`" + probe.argv[0] + "` was not found on PATH"
          : "`" + probe.argv[0] + " " + probe.argv[1] + "` failed: " + first_line(r.stderr_text);
      std::string others;
      for (pkg::manager m : pkg::all_managers()) {
        if (m != *chosen && dev::run_process(pkg::probe_command(m).argv).ok())
          others += (others.empty() ? "" : ", ") + std::string{pkg::manager_name(m)};
      }
      fail("the '" + args[0] + "' package manager is not available on this machine (" + why + ").\n     " +
           (others.empty() ? "No other supported package manager (" + pkg::manager_names() + ") was found either."
                           : "Available here: " + others + ". Run it with: wish client --run=pkg -- " +
                                 others.substr(0, others.find(','))));
      return;
    }
    version = first_line(r.stdout_text);
  } else {
    for (pkg::manager m : pkg::all_managers()) {
      auto r = dev::run_process(pkg::probe_command(m).argv);
      if (r.ok()) {
        chosen = m;
        version = first_line(r.stdout_text);
        break;
      }
    }
    if (!chosen) {
      fail("no supported package manager was found on this machine (looked for " + pkg::manager_names() +
           ").\n     Usage: " + kUsage);
      return;
    }
  }

  // ── How to get root ──
  pkg::elevation how = pkg::elevation::none;
  const std::string mode = args.size() > 1 ? args[1] : std::string{"auto"};
  if (!pkg::install_command(*chosen, {"x"}).needs_root) {
    // brew: nothing it does needs (or tolerates) root.
    if (mode != "auto" && !pkg::elevation_from_name(mode)) {
      fail("unknown elevation mode '" + mode + "'. Use sudo, pkexec, none or auto.\n     Usage: " + kUsage);
      return;
    }
  } else if (mode == "auto") {
    how = detect_elevation();
  } else if (auto named = pkg::elevation_from_name(mode)) {
    how = *named;
  } else {
    fail("unknown elevation mode '" + mode + "'. Use sudo, pkexec, none or auto.\n     Usage: " + kUsage);
    return;
  }

  // Every handler below runs as a job on the frontend's worker thread:
  // running the package manager inside an event handler would block the
  // whole UI until it exits. Long commands get a modal progress dialog
  // (common/command_worker.hpp).
  const auto frontend =
      dev::open_frontend(s, "PkgFrontend"_key, std::string{"Running "} + pkg::manager_name(*chosen));
  const auto& proxy = frontend.proxy;
  const auto& worker = frontend.worker;
  auto source = std::make_shared<pkg::pkg_source>(proxy, *chosen, how, worker);

  auto str = dev::payload_string; // optional payload string (absent -> "")

  worker->on(*proxy, "refresh_requested"_key, [source](dynamic) { source->refresh_all(); });
  worker->on(*proxy, "outdated_requested"_key, [source](dynamic) { source->on_outdated_requested(); });
  worker->on(*proxy, "index_requested"_key, [source](dynamic) { source->on_index_requested(); });
  worker->on(*proxy, "upgrade_all_requested"_key, [source](dynamic) { source->on_upgrade_all_requested(); });
  worker->on(*proxy, "search_requested"_key, [source, str](dynamic payload) {
    source->on_search_requested(str(payload, "query"_key));
  });
  worker->on(*proxy, "install_requested"_key, [source, str](dynamic payload) {
    source->on_install_requested(str(payload, "names"_key));
  });
  worker->on(*proxy, "package_action_requested"_key, [source, str](dynamic payload) {
    source->on_package_action(str(payload, "name"_key), str(payload, "action"_key));
  });
  worker->on(*proxy, "details_requested"_key, [source, str](dynamic payload) {
    source->on_details_requested(str(payload, "kind"_key), str(payload, "name"_key));
  });

  // Initial population -- queued here, now that every handler is registered,
  // rather than via a form-emitted event that would race ahead of this
  // wiring (the docker / git / kubectl initial-load-race fix).
  source->push_environment(version);
  worker->post([source] { source->refresh_all(); });
}

namespace {
struct pkg_app_registrar {
  pkg_app_registrar() {
    register_app({
        .name = "pkg",
        .organization = WISH_MODULE_BDG_DEV_PKG_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_PKG_COLLECTION,
        .description = "GUI frontend for the system package manager -- apt, dnf, pacman or brew: list, search, "
                       "install, upgrade and remove packages (wish client --run=pkg -- <manager>)",
        .params = {{"manager", "Package manager to use: apt, dnf, pacman or brew (optional; default: the first "
                               "one found on this machine)"},
                   {"elevation", "How commands that need root get it: sudo, pkexec, none or auto (optional; "
                                 "default: auto)"}},
        .run = run_pkg,
    });
  }
};
const pkg_app_registrar pkg_app_registrar_instance;
} // namespace

} // namespace bdg::wish
