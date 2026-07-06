// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-bridge binary.
#include "app/wish_cli/bridge/wish_bridge_app.hpp"

#include <gflags/gflags.h>

// verbose is declared by wish_bridge_app internally; define it here.
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-bridge - wish RMI bridge / multiplexer");
  return bdg::wish::wish_bridge_app::run(argc, argv);
}
