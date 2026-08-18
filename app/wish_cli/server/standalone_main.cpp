// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-server binary.
#include "app/wish_cli/server/wish_server_app.hpp"

#include <gflags/gflags.h>

#include <string>

// Shared transport flags — declared by bison::app::server_app internals.
DEFINE_string(transport, "term", "Transport to use: tcp, pipe, tls, or term");
DEFINE_string(host, "0.0.0.0", "Bind host address (transport=tcp/tls)");
DEFINE_int32(port, 7070, "Listen port (transport=tcp/tls)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_string(cmd, "", "Command to spawn (transport=term)");
DEFINE_bool(verbose, false, "Print session trace messages to stdout");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");
DEFINE_string(cert_file, "", "Server certificate chain file (transport=tls)");
DEFINE_string(cert_pem, "", "Server certificate chain PEM (transport=tls)");
DEFINE_string(key_file, "", "Server private key file (transport=tls)");
DEFINE_string(key_pem, "", "Server private key PEM (transport=tls)");
DEFINE_string(key_password, "", "Passphrase for an encrypted server private key (transport=tls)");
DEFINE_string(client_auth, "none", "Mutual TLS mode: none, optional, or required (transport=tls)");
DEFINE_string(ca_file, "", "Trust anchor file for verifying client certificates (transport=tls, client_auth!=none)");
DEFINE_string(ca_pem, "", "Trust anchor PEM for verifying client certificates (transport=tls, client_auth!=none)");
// Not validated here -- an unrecognized name falls back to the renderer's
// default theme with a logged warning; see style_service.hpp's "Supported
// preset names".
DEFINE_string(theme, "wish",
    "Default UI theme preset for connecting clients that don't request their own via "
    "--theme, e.g. dark, light, classic, or wish.");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-server - wish GUI render server");
  bdg::wish::wish_server_app app;
  return app.run(argc, argv);
}
