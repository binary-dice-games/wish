// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-desktop binary.
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include <gflags/gflags.h>

// Shared transport flags — declared by bison::app::bridge_app internals.
DEFINE_string(transport, "tcp", "Downstream transport to use: tcp, pipe, or term");
DEFINE_string(host, "0.0.0.0", "Downstream bind host address (downstream_transport=tcp)");
DEFINE_int32(port, 7071, "Downstream listen port (downstream_transport=tcp)");
DEFINE_string(name, "", "Downstream named-pipe / Unix-socket path (downstream_transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (downstream_transport=term)");
DEFINE_int32(timeout, 30000, "Upstream per-request timeout in milliseconds");
DEFINE_string(verbose, "none", "Log verbosity: none|fatal|error|warning|info|trace");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-desktop - wish RMI bridge / multiplexer with desktop shell");
  bdg::wish::wish_desktop_app app;
  // Apps launched from the desktop run through the wish-client binary
  // installed beside this one.
  auto self = bdg::wish::process_launcher::self_exe_path();
  app.set_client_command({(self.parent_path() / ("wish-client" + self.extension().string())).string()});
  return app.run(argc, argv);
}
