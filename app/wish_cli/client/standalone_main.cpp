// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-client binary.
#include "app/wish_cli/client/wish_client_app.hpp"

#include <gflags/gflags.h>

// Shared flags (--pipe is defined in wish_client_app.cpp).
DEFINE_string(host, "0.0.0.0", "Connect host address");
DEFINE_int32(port, 7070, "Connect port");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-client - wish GUI remote client");
  return bdg::wish::run_client_mode(argc, argv);
}
