// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-server binary.
#include "app/wish_cli/server/wish_server_app.hpp"

#include <gflags/gflags.h>

// Shared transport flags — declared by bison::app::server_app internals.
DEFINE_string(transport, "term", "Transport to use: tcp, pipe or term");
DEFINE_string(host, "0.0.0.0", "Bind host address (transport=tcp)");
DEFINE_int32(port, 7070, "Listen port (transport=tcp)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=console)");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-server - wish GUI render server");
  bdg::wish::wish_server_app app;
  return app.run(argc, argv);
}
