// MIT License © 2025 Binary Dice Games
/// @file standalone_main.cpp
/// @brief Entry point for the standalone wish-client binary.
#include "app/wish_cli/client/wish_client_app.hpp"

#include <gflags/gflags.h>

#include <string>

// Shared transport flags — declared by bison::app internals.
DEFINE_string(transport, "term", "Transport to use: tcp, pipe, tls, or term");
DEFINE_string(host, "0.0.0.0", "Connect host address (transport=tcp/tls)");
DEFINE_int32(port, 7070, "Connect port (transport=tcp/tls)");
DEFINE_string(name, "", "Named-pipe / Unix-socket path (transport=pipe)");
DEFINE_bool(debugger, false, "Wait for debugger attachment before starting");
DEFINE_string(ca_file, "", "Trust anchor file for verifying the server's certificate (transport=tls)");
DEFINE_string(ca_pem, "", "Trust anchor PEM for verifying the server's certificate (transport=tls)");
DEFINE_string(server_name, "", "SNI / hostname-verification target (transport=tls, default: --host)");
DEFINE_bool(insecure_skip_verify, false, "Skip server certificate verification -- unsafe, dev/test only (transport=tls)");
DEFINE_string(cert_file, "", "Client certificate file, for mutual TLS (transport=tls)");
DEFINE_string(cert_pem, "", "Client certificate PEM, for mutual TLS (transport=tls)");
DEFINE_string(key_file, "", "Client private key file, for mutual TLS (transport=tls)");
DEFINE_string(key_pem, "", "Client private key PEM, for mutual TLS (transport=tls)");
DEFINE_string(key_password, "", "Passphrase for an encrypted client private key (transport=tls)");
// Not validated here -- an unrecognized name falls back to the renderer's
// default theme with a logged warning; see style_service.hpp's "Supported
// preset names".
DEFINE_string(theme, "wish", "UI theme preset, e.g. dark, light, classic, or wish.");

int main(int argc, char** argv) {
  gflags::SetUsageMessage("wish-client - wish GUI remote client");
  bdg::wish::wish_client_app app;
  return app.run(argc, argv);
}
