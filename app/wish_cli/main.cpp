// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the wish CLI — dispatches server / client / bridge.
 *
 * Usage:
 *   wish server  [--transport T] [--host H] [--port P] [--name PATH] [--cmd C]
 *                [--title T] [--width W] [--height H] [--verbose]
 *   wish client  [--host H] [--port P] [--pipe PATH]
 *                (--list | --run=<app>) [--timeout MS]
 *   wish bridge  [--up-host H] [--up-port P] [--up-pipe PATH]
 *                [--down-host H] [--down-port P] [--down-pipe PATH]
 */
#include "app/wish_cli/bridge/wish_bridge_app.hpp"
#include "app/wish_cli/client/wish_client_app.hpp"
#include "app/wish_cli/server/wish_server_app.hpp"

#include <gflags/gflags.h>

#include <cstring>
#include <iostream>

// ── Shared flags — consumed by server and client modes ────────────────────────
DEFINE_string(host, "0.0.0.0", "Bind/connect host address");
DEFINE_int32(port, 7070, "Listen/connect port");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

// ── Server-only transport flags — consumed by bison::app::server_app ─────────
DEFINE_string(transport, "tcp", "Transport to use: tcp, pipe, pty, or console");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=console)");

static void print_usage() {
  std::cout << "wish - remote GUI framework CLI\n"
               "\n"
               "Usage:\n"
               "  wish server  [--transport T] [--host H] [--port P] [--name PATH] [--cmd C]\n"
               "               [--title T] [--width W] [--height H] [--verbose]\n"
               "  wish client  [--host H] [--port P] [--pipe PATH] (--list | --run=<app>) [--timeout MS]\n"
               "  wish bridge  [--up-host H --up-port P ...] [--down-host H --down-port P ...]\n"
               "\n"
               "Server transport flags:\n"
               "  --transport T  tcp, pipe, pty, or console (default: tcp)\n"
               "  --host H       Bind host address          (default: 0.0.0.0)\n"
               "  --port P       Bind port                   (default: 7070)\n"
               "  --name PATH    Named-pipe / Unix-socket path (transport=pipe)\n"
               "  --cmd C        Command to spawn             (transport=console)\n"
               "  --verbose      Print RMI trace messages\n"
               "  --debugger     Wait for debugger attachment before starting\n"
               "\n"
               "Run 'wish <subcommand> --help' for subcommand-specific flags.\n";
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }

  const char* subcmd = argv[1];

  // Shift argv so gflags sees only the subcommand's arguments.
  // argv[0] stays as the program name; subcmd is removed.
  argv[1] = argv[0];
  int sub_argc = argc - 1;
  char** sub_argv = argv + 1;

  if (std::strcmp(subcmd, "server") == 0) {
    gflags::SetUsageMessage("wish server - render server that accepts client connections");
    bdg::wish::wish_server_app app;
    return app.run(sub_argc, sub_argv);
  }

  if (std::strcmp(subcmd, "client") == 0) {
    gflags::SetUsageMessage("wish client - connect to a server and run an embedded application");
    return bdg::wish::run_client_mode(sub_argc, sub_argv);
  }

  if (std::strcmp(subcmd, "bridge") == 0) {
    gflags::SetUsageMessage("wish bridge - multiplexing bridge with upstream/downstream transports");
    return bdg::wish::wish_bridge_app::run(sub_argc, sub_argv);
  }

  std::cerr << "wish: unknown subcommand '" << subcmd << "'\n\n";
  print_usage();
  return 1;
}
