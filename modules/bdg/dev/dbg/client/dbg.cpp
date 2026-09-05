// MIT License © 2026 Binary Dice Games
/// @file dbg.cpp
/// @brief Client-side runner for the dbg (DebuggerFrontend) embedded app.
///
/// `wish client --run=dbg` -- instantiates the DebuggerFrontend form and
/// wires its `*_requested` events to a dbg_source, which drives a
/// debug_backend. Step 3 (PLAN.md) has no real backend yet -- that lands in
/// Step 4 as win32_debug_backend; until then this module has no usable
/// `.run` entry point wired to a concrete backend, so run_dbg() is exercised
/// directly by tests/test_dbg.cpp against a synthetic fake debug_backend
/// rather than through the registrar below.
#include "dbg.hpp"
#include "dbg_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

namespace bdg::wish {

using namespace bison;

void run_dbg(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "DebuggerFrontend"_key).get());

  // No debug_backend implementation exists yet (win32_debug_backend is
  // PLAN.md Step 4), so this runner cannot construct a dbg_source. Once
  // Step 4 lands, this becomes:
  //   auto source = std::make_shared<dbg::dbg_source>(proxy,
  //       std::make_unique<dbg::win32_debug_backend>());
  //   proxy->onEvent("attach_requested"_key, ...); etc.
  //   proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });
  proxy->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });
}

namespace {
struct dbg_app_registrar {
  dbg_app_registrar() {
    register_app({
        .name = "dbg",
        .organization = WISH_MODULE_BDG_DEV_DBG_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_DBG_COLLECTION,
        .description = "Visual-Studio-style debugger GUI frontend (wish client --run=dbg)",
        .params = {},
        .run = run_dbg,
    });
  }
};
const dbg_app_registrar dbg_app_registrar_instance;
} // namespace

} // namespace bdg::wish
