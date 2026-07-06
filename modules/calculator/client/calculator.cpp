// MIT License © 2025 Binary Dice Games
/// @file calculator.cpp
/// @brief Client-side runner for the Calculator embedded app.
#include "modules/calculator/client/calculator.hpp"

#include "app/wish_cli/client/app_registry.hpp"
#include "app/wish_cli/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

namespace bdg::wish {

using namespace bison;

/// Instantiate the server-side Calculator form, register for the "closed"
/// event, and keep the proxy alive until signal_done() fires.
void run_calculator(wish_app_host& s) {
  auto proxy = s.instantiate("wish"_key, "Calculator"_key).get();

  // When the user clicks "Close" inside the form, the server emits "closed".
  // Capture a raw pointer to the session; it is valid for the whole session.
  proxy.onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  s.keep_alive(std::move(proxy));
  // on_session() blocks until signal_done() is called.
}

namespace {
struct calculator_app_registrar {
  calculator_app_registrar() {
    register_app({
        .name = "calculator",
        .description = "Four-function calculator; demonstrates self-contained form logic",
        .params = {},
        .run = run_calculator,
    });
  }
};
const calculator_app_registrar calculator_app_registrar_instance;
} // namespace

} // namespace bdg::wish
