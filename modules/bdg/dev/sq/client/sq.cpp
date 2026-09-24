// MIT License © 2026 Binary Dice Games
/// @file sq.cpp
/// @brief Client-side runner for the sq (SqFrontend) embedded app.
///
/// `wish client --run=sq` -- no positional args. Every command runs the
/// user's own `sq` binary against the sq configuration it would use from the
/// shell. Owns a sq_source (all `sq` invocation + parsing) and wires the
/// form's `*_requested` events to it -- see server/sq.hpp for the contract.
///
/// If `sq` is not installed the form is still shown, with a message and the
/// project URL, instead of failing silently.
#include "sq.hpp"
#include "sq_process.hpp"
#include "sq_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <iostream>
#include <memory>

namespace bdg::wish {

using namespace bison;

namespace {
constexpr const char* kSqRepoUrl = "https://github.com/neilotoole/sq";
}

void run_sq(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "SqFrontend"_key).get());
  proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  auto check = sq::run_sq_cli({"version"});
  if (!check.ok()) {
    const std::string message = std::string{"The `sq` command line tool was not found on your PATH.\n"
                                            "Install it (see the Install section of the README), then restart "
                                            "this tool:\n"} +
        kSqRepoUrl;
    std::cerr << "sq: " << message << "\n";
    dynamic args;
    args["message"_key] = message;
    args["url"_key] = std::string{kSqRepoUrl};
    proxy->call("show_unavailable"_key, std::move(args)).get();
    return;
  }

  auto source = std::make_shared<sq::sq_source>(proxy);

  proxy->onEvent("refresh_requested"_key, [source](dynamic) { source->refresh_all(); });
  proxy->onEvent("activate_requested"_key, [source](dynamic p) {
    source->on_activate(p.as<std::string>("handle"_key));
  });
  proxy->onEvent("ping_requested"_key, [source](dynamic p) { source->on_ping(p.as<std::string>("handle"_key)); });
  proxy->onEvent("remove_connection_requested"_key, [source](dynamic p) {
    source->on_remove(p.as<std::string>("handle"_key));
  });
  proxy->onEvent("add_connection_requested"_key, [source](dynamic p) {
    source->on_add(
        p.as<std::string>("handle"_key), p.as<std::string>("location"_key), p.as<std::string>("driver"_key),
        p.as<std::string>("password"_key));
  });
  proxy->onEvent("query_requested"_key, [source](dynamic p) {
    source->on_query(p.as<std::string>("sql"_key), p.as<int32_t>("max_rows"_key));
  });
  proxy->onEvent("export_requested"_key, [source](dynamic p) {
    source->on_export(p.as<std::string>("path"_key), p.as<bool>("overwrite"_key));
  });

  // Initial population, called directly now that every handler is wired
  // (never via a form-emitted event that would race ahead of the wiring).
  source->refresh_all();
}

namespace {
struct sq_app_registrar {
  sq_app_registrar() {
    register_app({
        .name = "sq",
        .organization = WISH_MODULE_BDG_DEV_SQ_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_SQ_COLLECTION,
        .description = "DBeaver-style, query-only GUI frontend for the local `sq` CLI (wish client --run=sq)",
        .params = {},
        .run = run_sq,
    });
  }
};
const sq_app_registrar sq_app_registrar_instance;
} // namespace

} // namespace bdg::wish
