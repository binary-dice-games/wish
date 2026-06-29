// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-client binary.
#include "app/wish_cli/client/wish_client_app.hpp"

#include <gflags/gflags.h>

// Shared transport flags — declared by bison::app::client_app internals.
DEFINE_string(host, "0.0.0.0", "Connect host address");
DEFINE_int32 (port, 7070,      "Connect port");
DEFINE_string(pipe, "",        "Named pipe / Unix socket path");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-client - wish GUI remote client");
  return bdg::wish::run_client_mode(argc, argv);
}
