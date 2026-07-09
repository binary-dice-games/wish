// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-standalone binary.
#include "app/wish_cli/standalone/wish_standalone_app.hpp"

#include <gflags/gflags.h>

// wish_standalone_app.cpp only DECLAREs these (normally defined by main.cpp /
// wish_server_app.cpp / wish_client_app.cpp); define them here so this binary
// is self-contained. transport/host/port/name are never valid in standalone
// mode -- they exist only so run_standalone_mode()'s rejection check can
// look them up and report a clear error if the user passes one.
DEFINE_string(transport, "term", "Not supported in standalone mode");
DEFINE_string(host, "0.0.0.0", "Not supported in standalone mode");
DEFINE_int32(port, 7070, "Not supported in standalone mode");
DEFINE_string(name, "", "Not supported in standalone mode");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

DEFINE_string(title, "wish", "Window title");
DEFINE_int32(width, 1280, "Window width in pixels");
DEFINE_int32(height, 720, "Window height in pixels");

DEFINE_bool(list, false, "List available embedded applications and exit");
DEFINE_string(run, "", "Name of the embedded application to run");
DEFINE_string(describe, "", "Print name, description, and parameters for a specific embedded application and exit");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-standalone - in-process wish server+client, no transport");
  return bdg::wish::run_standalone_mode(argc, argv);
}
