// MIT License © 2025 Binary Dice Games
/// @file bc.cpp
/// @brief Client-side runner for the bc embedded app.
#include "modules/bdg/desktop/bc/client/bc.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

namespace bdg::wish {

using namespace bison;

/// Instantiate the server-side bc form, register for the "closed"
/// event, and keep the proxy alive until signal_done() fires.
void run_bc(wish_app_host& s) {
  auto proxy = s.instantiate("wish"_key, "Bc"_key).get();

  // When the user clicks "Close" inside the form, the server emits "closed".
  // Capture a raw pointer to the session; it is valid for the whole session.
  proxy.onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  s.keep_alive(std::move(proxy));
  // on_session() blocks until signal_done() is called.
}

namespace {
struct bc_app_registrar {
  bc_app_registrar() {
    register_app({
        .name = "bc",
        .organization = WISH_MODULE_BDG_DESKTOP_BC_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_BC_COLLECTION,
        .description = "Four-function calculator; demonstrates self-contained form logic",
        .params = {},
        .run = run_bc,
    });
  }
};
const bc_app_registrar bc_app_registrar_instance;
} // namespace

} // namespace bdg::wish
