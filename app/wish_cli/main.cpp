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
 *   wish <app>      [--transport T] [--host H] [--port P] [--name PATH]
 *                   [--timeout MS] [-- app-args...]
 *                   Alias for `wish client --run=<app> ...`, available when
 *                   <app> is not one of the subcommands above and matches a
 *                   name in the client app registry (see `wish client --list`).
 */
#include "app/wish_cli/desktop/wish_desktop_app.hpp"
#include "src/client/app_registry.hpp"
#include "app/wish_cli/client/wish_client_app.hpp"
#include "app/wish_cli/server/wish_server_app.hpp"
#include "app/wish_cli/standalone/wish_standalone_app.hpp"

#include <gflags/gflags.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ── Shared transport flags — consumed by server and client modes ─────────────
DEFINE_string(transport, "term", "Transport to use: tcp, pipe, tls, or term");
DEFINE_string(host, "0.0.0.0", "Bind/connect host address");
DEFINE_int32(port, 7070, "Listen/connect port");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");

// ── Server-only flags — consumed by bison::app::server_app ───────────────────
DEFINE_string(cmd, "", "Command to spawn (transport=term)");

// ── TLS flags — shared by server (listen) and client (connect) modes,
//    consumed by bison::app::server_app/client_app (transport=tls) ───────────
DEFINE_string(cert_file, "", "Certificate chain file: server cert (server) or client cert for mutual TLS (client) (transport=tls)");
DEFINE_string(cert_pem, "", "Certificate chain PEM, alternative to --cert_file (transport=tls)");
DEFINE_string(key_file, "", "Private key file matching --cert_file/--cert_pem (transport=tls)");
DEFINE_string(key_pem, "", "Private key PEM, alternative to --key_file (transport=tls)");
DEFINE_string(key_password, "", "Passphrase for an encrypted private key (transport=tls)");
DEFINE_string(client_auth, "none", "Server mutual TLS mode: none, optional, or required (server, transport=tls)");
DEFINE_string(ca_file, "", "Trust anchor file: verifies client certs (server) or the server's cert (client) (transport=tls)");
DEFINE_string(ca_pem, "", "Trust anchor PEM, alternative to --ca_file (transport=tls)");
DEFINE_string(server_name, "", "SNI / hostname-verification target (client, transport=tls, default: --host)");
DEFINE_bool(insecure_skip_verify, false, "Skip server certificate verification -- unsafe, dev/test only (client, transport=tls)");

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
               "  wish <app>      [--transport T] [--host H] [--port P] [--name PATH]\n"
               "                  [--timeout MS] [-- app-args...]\n"
               "                  Alias for `wish client --run=<app> ...`, when <app> is not\n"
               "                  one of the subcommands above and names a registered app\n"
               "                  (see `wish client --list`).\n"
               "\n"
               "Anything after a literal `--` is forwarded to the app, e.g.\n"
               "  wish client --run=notepad -- path/to/file\n"
               "  wish notepad -- path/to/file\n"
               "\n"
               "Shared transport flags (server and client only -- wish desktop has its own\n"
               "downstream_/upstream_-prefixed flag sets, see below):\n"
               "  --transport T  tcp, pipe, tls, or term      (default: term)\n"
               "  --host H       Host address (default: 0.0.0.0 for server, 127.0.0.1 for client)\n"
               "  --port P       Port                        (default: 7070)\n"
               "  --name PATH    Named-pipe / Unix-socket path (transport=pipe)\n"
               "  --cmd C        Command to spawn             (transport=term, server only)\n"
               "  --verbose      Print RMI trace messages\n"
               "  --debugger     Wait for debugger attachment before starting\n"
               "\n"
               "TLS flags (--transport=tls, see docs/cli.md and docs/tls.md for the full\n"
               "reference):\n"
               "  --cert_file/--cert_pem, --key_file/--key_pem, --key_password\n"
               "                 Server certificate/key (server); optional client\n"
               "                 certificate/key for mutual TLS (client)\n"
               "  --client_auth  none (default) | optional | required -- server mutual TLS mode\n"
               "  --ca_file/--ca_pem\n"
               "                 Trust anchor: verifies client certs (server, when\n"
               "                 --client_auth != none) or the server's cert (client)\n"
               "  --server_name  SNI / hostname-verification target (client, default: --host)\n"
               "  --insecure_skip_verify\n"
               "                 Skip server certificate verification -- unsafe, dev/test only (client)\n"
               "\n"
               "wish desktop has two transports active at once, so each side gets its own\n"
               "explicitly-prefixed flag set to avoid ambiguity -- downstream (accepting\n"
               "client connections):\n"
               "  --downstream_transport T  tcp, pipe, tls, or term  (default: tcp)\n"
               "  --downstream_host H       Downstream host     (default: 0.0.0.0)\n"
               "  --downstream_port P       Downstream port     (default: 7070)\n"
               "  --downstream_name PATH    Downstream named-pipe / Unix-socket path (downstream_transport=pipe)\n"
               "  --downstream_cert_file/--downstream_cert_pem, --downstream_key_file/\n"
               "  --downstream_key_pem, --downstream_key_password, --downstream_client_auth,\n"
               "  --downstream_ca_file/--downstream_ca_pem\n"
               "                            Downstream TLS server flags (downstream_transport=tls)\n"
               "\n"
               "...and upstream (the single relayed connection):\n"
               "  --upstream_transport T  tcp, pipe, tls, or term  (default: term)\n"
               "  --upstream_host H       Upstream host       (default: 127.0.0.1)\n"
               "  --upstream_port P       Upstream port       (default: 7070)\n"
               "  --upstream_name PATH    Upstream named-pipe / Unix-socket path (upstream_transport=pipe)\n"
               "  --upstream_ca_file/--upstream_ca_pem, --upstream_server_name,\n"
               "  --upstream_insecure_skip_verify, --upstream_cert_file/--upstream_cert_pem,\n"
               "  --upstream_key_file/--upstream_key_pem, --upstream_key_password\n"
               "                          Upstream TLS client flags (upstream_transport=tls)\n"
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

  // Not a known subcommand -- if it names a registered app, alias it to
  // `wish client --run=<subcmd> ...`, forwarding the remaining original
  // arguments (transport flags, `-- app-args`, etc.) unchanged.
  if (bdg::wish::registered_apps().count(subcmd) != 0) {
    gflags::SetUsageMessage("wish client - connect to a server and run an embedded application");
    std::vector<std::string> alias_args;
    alias_args.reserve(static_cast<size_t>(argc));
    alias_args.emplace_back(argv[0]);
    alias_args.emplace_back(std::string("--run=") + subcmd);
    for (int i = 2; i < argc; ++i)
      alias_args.emplace_back(argv[i]);

    std::vector<char*> alias_argv;
    alias_argv.reserve(alias_args.size());
    for (auto& arg : alias_args)
      alias_argv.push_back(arg.data());

    bdg::wish::wish_client_app app;
    return app.run(static_cast<int>(alias_argv.size()), alias_argv.data());
  }

  std::cerr << "wish: unknown subcommand '" << subcmd << "'\n\n";
  print_usage();
  return 1;
}
