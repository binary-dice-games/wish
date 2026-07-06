// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-client binary.
#include "app/wish_cli/client/wish_client_app.hpp"

#include <gflags/gflags.h>

// Shared transport flags — declared by bison::app internals.
DEFINE_string(transport, "tcp", "Transport to use: tcp, pipe, pty, or console");
DEFINE_string(host, "0.0.0.0", "Connect host address (transport=tcp)");
DEFINE_int32(port, 7070, "Connect port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-client - wish GUI remote client");
  bdg::wish::wish_client_app app;
  return app.run(argc, argv);
}
