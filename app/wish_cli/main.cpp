// MIT License © 2025 Binary Dice Games
/**
 * @file main.cpp
 * @brief Entry point for the wish CLI — dispatches server / client / desktop /
 *        standalone.
 *
 * Usage:
 *   wish server     [--transport T] [--host H] [--port P] [--name PATH] [--cmd C]
 *                   [--title T] [--width W] [--height H] [--verbose]
 *   wish client     [--transport T] [--host H] [--port P] [--name PATH]
 *                   (--list | --run=<app>) [--timeout MS] [-- app-args...]
 *   wish standalone [--title T] [--width W] [--height H]
 *                   (--list | --run=<app>) [-- app-args...]
 *                   (--transport/--host/--port/--name are not accepted here —
 *                   standalone mode runs server and client in one process)
 *   wish desktop    [--downstream_transport T] [--downstream_host H]
 *                   [--downstream_port P] [--downstream_name PATH]
 *                   [--upstream_transport T] [--upstream_host H]
 *                   [--upstream_port P] [--upstream_name PATH] [--timeout MS]
 */
#include "app/wish_cli/desktop/wish_desktop_app.hpp"
#include "app/wish_cli/client/wish_client_app.hpp"
#include "app/wish_cli/server/wish_server_app.hpp"
#include "app/wish_cli/standalone/wish_standalone_app.hpp"

#include <gflags/gflags.h>

#include <cstring>
#include <iostream>

// ── Shared transport flags — consumed by server and client modes ─────────────
DEFINE_string(transport, "term", "Transport to use: tcp, pipe or term");
DEFINE_string(host, "0.0.0.0", "Bind/connect host address");
DEFINE_int32(port, 7070, "Listen/connect port");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

// ── Server-only flags — consumed by bison::app::server_app ───────────────────
DEFINE_string(cmd, "", "Command to spawn (transport=console)");

static void print_usage() {
  std::cout << "wish - remote GUI framework CLI\n"
               "\n"
               "Usage:\n"
               "  wish server     [--transport T] [--host H] [--port P] [--name PATH] [--cmd C]\n"
               "                  [--title T] [--width W] [--height H] [--verbose]\n"
               "  wish client     [--transport T] [--host H] [--port P] [--name PATH]\n"
               "                  (--list | --run=<app>) [--timeout MS] [-- app-args...]\n"
               "  wish standalone [--title T] [--width W] [--height H]\n"
               "                  (--list | --run=<app>) [-- app-args...]\n"
               "  wish desktop    [--downstream_transport T] [--downstream_host H]\n"
               "                  [--downstream_port P] [--downstream_name PATH]\n"
               "                  [--upstream_transport T] [--upstream_host H] [--upstream_port P]\n"
               "                  [--upstream_name PATH] [--timeout MS]\n"
               "\n"
               "Anything after a literal `--` is forwarded to the app, e.g.\n"
               "  wish client --run=notepad -- path/to/file\n"
               "\n"
               "Shared transport flags (server and client only -- wish desktop has its own\n"
               "downstream_/upstream_-prefixed flag sets, see below):\n"
               "  --transport T  tcp, pipe, pty, or console (default: tcp)\n"
               "  --host H       Host address (default: 0.0.0.0 for server, 127.0.0.1 for client)\n"
               "  --port P       Port                        (default: 7070)\n"
               "  --name PATH    Named-pipe / Unix-socket path (transport=pipe)\n"
               "  --cmd C        Command to spawn             (transport=console, server only)\n"
               "  --verbose      Print RMI trace messages\n"
               "  --debugger     Wait for debugger attachment before starting\n"
               "\n"
               "wish desktop has two transports active at once, so each side gets its own\n"
               "explicitly-prefixed flag set to avoid ambiguity -- downstream (accepting\n"
               "client connections):\n"
               "  --downstream_transport T  tcp, pipe, or term  (default: tcp)\n"
               "  --downstream_host H       Downstream host     (default: 0.0.0.0)\n"
               "  --downstream_port P       Downstream port     (default: 7070)\n"
               "  --downstream_name PATH    Downstream named-pipe / Unix-socket path (downstream_transport=pipe)\n"
               "\n"
               "...and upstream (the single relayed connection):\n"
               "  --upstream_transport T  tcp, pipe, or term  (default: term)\n"
               "  --upstream_host H       Upstream host       (default: 127.0.0.1)\n"
               "  --upstream_port P       Upstream port       (default: 7070)\n"
               "  --upstream_name PATH    Upstream named-pipe / Unix-socket path (upstream_transport=pipe)\n"
               "\n"
               "wish standalone runs the server and client in the same process (no\n"
               "transport, no serialization) and does not accept --transport/--host/\n"
               "--port/--name.\n"
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
    bdg::wish::wish_client_app app;
    return app.run(sub_argc, sub_argv);
  }

  if (std::strcmp(subcmd, "standalone") == 0) {
    gflags::SetUsageMessage("wish standalone - in-process server+client, no transport");
    return bdg::wish::run_standalone_mode(sub_argc, sub_argv);
  }

  if (std::strcmp(subcmd, "desktop") == 0) {
    gflags::SetUsageMessage("wish desktop - multiplexing bridge with upstream/downstream transports");
    bdg::wish::wish_desktop_app app;
    return app.run(sub_argc, sub_argv);
  }

  std::cerr << "wish: unknown subcommand '" << subcmd << "'\n\n";
  print_usage();
  return 1;
}
