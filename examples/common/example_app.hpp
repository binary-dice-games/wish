// MIT License © 2025 Binary Dice Games
//
// @file example_app.hpp
// @brief Shared boilerplate for single-binary wish examples (flag
//        definitions, renderer selection, and the client/server run loop).
//        Each example still owns its UI descriptor and event wiring; only the
//        parts that are identical across examples (flags, main(), the
//        verbose-log helper) live here.

#pragma once

#include <client/client.hpp>
#include <server/renderer.hpp>

#include "src/rmi/rmi.hpp" // memory_server_transport / memory_client_transport

#include <gflags/gflags.h>

#include <functional>
#include <memory>
#include <string>

DECLARE_bool(verbose);
DECLARE_string(theme);
DECLARE_int32(font_size);
DECLARE_string(renderer);
DECLARE_int32(web_port);
DECLARE_string(web_bind);

namespace bdg::wish::examples {

// Builds the renderer requested by --renderer, sized for `window_title`.
std::unique_ptr<renderer> make_renderer(const std::string& window_title, int width, int height);

// ── Client base ───────────────────────────────────────────────────────────

// Common state and logging helper every example client needs: the renderer
// (to poll should_quit()), the --verbose/--theme flag values, and a
// `[tag] message` trace line gated on --verbose.
class example_client : public wish::client {
 public:
  example_client(
      bison::rmi::transport::memory_client_transport t,
      wish::renderer* renderer,
      bool verbose,
      std::string theme,
      std::string tag);

 protected:
  void vlog(const std::string& msg) const;

  wish::renderer* renderer_;
  bool verbose_;
  std::string theme_;

 private:
  std::string tag_;
};

// Constructs a `ClientT` (a subclass of example_client) from the connected
// transport plus the --verbose/--theme flag values.
using client_factory = std::function<std::unique_ptr<example_client>(
    bison::rmi::transport::memory_client_transport,
    wish::renderer*,
    bool verbose,
    std::string theme)>;

// ── main() driver ─────────────────────────────────────────────────────────

// Parses flags, wires up an in-memory transport + renderer + server, runs
// the client produced by `make_client` to completion, then stops the server.
int run_example(
    int argc,
    char* argv[],
    const char* tag,
    const std::string& window_title,
    int width,
    int height,
    bool allow_absolute_paths,
    const client_factory& make_client);

} // namespace bdg::wish::examples
