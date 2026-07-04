// MIT License © 2025 Binary Dice Games
/// @file process_explorer.cpp
/// @brief Client-side runner for the Process Explorer embedded app.
#include "app/wish_cli/client/apps/process_explorer.hpp"
#include "app/wish_cli/client/wish_client_app.hpp"

#include "src/bison/bison.hpp"

namespace bdg::wish {

using namespace bison;

/// Instantiate the server-side ProcessExplorer form, register for the
/// "closed" event, and keep the proxy alive until signal_done() fires.
/// All process/CPU/memory sampling and UI updates happen server-side; the
/// client has nothing else to do.
void run_process_explorer(wish_client_session& s) {
  auto proxy = s.instantiate("wish"_key, "ProcessExplorer"_key).get();

  proxy.onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  s.keep_alive(std::move(proxy));
  // on_session() blocks until signal_done() is called.
}

} // namespace bdg::wish
