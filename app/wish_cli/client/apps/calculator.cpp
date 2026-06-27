// MIT License © 2025 Binary Dice Games
/// @file calculator.cpp
/// @brief Client-side runner for the Calculator embedded app.
#include "app/wish_cli/client/apps/calculator.hpp"
#include "app/wish_cli/client/wish_client_app.hpp"

#include "src/bison/bison.hpp"

namespace bdg::wish {

using namespace bison;

/// Instantiate the server-side Calculator form, register for the "closed"
/// event, and keep the proxy alive until signal_done() fires.
void run_calculator(wish_client_session& s) {
  auto proxy = s.instantiate("wish"_key, "Calculator"_key).get();

  // When the user clicks "Close" inside the form, the server emits "closed".
  // Capture a raw pointer to the session; it is valid for the whole session.
  proxy.onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  s.keep_alive(std::move(proxy));
  // on_session() blocks until signal_done() is called.
}

} // namespace bdg::wish
