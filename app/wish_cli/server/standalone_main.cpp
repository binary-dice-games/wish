// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-server binary.
#include "app/wish_cli/server/wish_server_app.hpp"

#include <gflags/gflags.h>

// Shared transport flags — declared by bison::app::server_app internals.
DEFINE_string(host,    "0.0.0.0", "Bind host address");
DEFINE_int32 (port,    7070,      "Listen port");
DEFINE_string(pipe,    "",        "Named pipe / Unix socket path");
DEFINE_bool  (verbose, false,     "Print session trace messages to stdout");
#if defined(__linux__)
DEFINE_bool  (pty, false, "Use PTY transport (Linux only)");
#endif

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-server - wish GUI render server");
  bdg::wish::wish_server_app app;
  return app.run(argc, argv);
}
