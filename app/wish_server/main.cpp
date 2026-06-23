// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the wish GUI server.
 */
#include "app/wish_server/wish_server.hpp"

#include <gflags/gflags.h>

// ── Transport flags (consumed by srv_app) ─────────────────────────────────────
DEFINE_string(host,    "0.0.0.0", "Bind host address");
DEFINE_int32 (port,    7070,      "Listen port");
DEFINE_string(pipe,    "",        "Named-pipe / Unix-socket path (empty = use socket)");
DEFINE_bool  (pty,     false,     "Use PTY transport (Linux only)");
DEFINE_bool  (verbose, false,     "Print session trace messages to stdout");

// ── Window flags (consumed by wish_server) ────────────────────────────────────
DEFINE_string(title,  "wish", "Window title");
DEFINE_int32 (width,  1280,   "Window width in pixels");
DEFINE_int32 (height, 720,    "Window height in pixels");

#if defined(__linux__)
DEFINE_string(cmd, "bash", "Shell command spawned for PTY transport");
#endif

int main(int argc, char** argv) {
  gflags::SetUsageMessage(
      "wish — GUI server for the wish remote UI framework\n"
      "Opens an SDL3 window and accepts client connections.");
  bdg::wish::wish_server server;
  return server.run(argc, argv);
}
