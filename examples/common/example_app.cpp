// MIT License © 2025 Binary Dice Games
//
// @file example_app.cpp
// @brief Implementation of the shared example boilerplate declared in
//        example_app.hpp.

#include "example_app.hpp"

#include <sdl/sdl3_renderer.hpp>
#include <server/server.hpp>
#include <web/web_renderer.hpp>

#include <iostream>

DEFINE_bool(verbose, false, "Print verbose trace to stderr.");
DEFINE_string(theme, "wish", "UI theme preset: wish, dark, light, or classic.");
DEFINE_int32(font_size, 16, "Font size in pixels");
DEFINE_string(renderer, "web", "Rendering backend: sdl3 or web");
DEFINE_int32(web_port, 8080, "HTTP/WebSocket port for --renderer web");
DEFINE_string(web_bind, "127.0.0.1", "Bind address for --renderer web (localhost-only by default)");

namespace {
bool ValidateTheme(const char* /*flag*/, const std::string& value) {
  return value == "wish" || value == "dark" || value == "light" || value == "classic";
}
} // namespace
DEFINE_validator(theme, &ValidateTheme);

namespace bdg::wish::examples {

std::unique_ptr<renderer> make_renderer(const std::string& window_title, int width, int height) {
  if (FLAGS_renderer == "sdl3") {
#ifdef WISH_SDL3_ENABLED
    return std::make_unique<sdl3_renderer>(window_title.c_str(), width, height, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=sdl3 requested but this binary was built with WISH_ENABLE_SDL3=OFF");
#endif
  }
  if (FLAGS_renderer == "web") {
#ifdef WISH_WEB_ENABLED
    std::cout << "[wish] open http://" << FLAGS_web_bind << ':' << FLAGS_web_port << " in a browser\n"
              << "Press Ctrl+C to stop\n"
              << std::flush;
    return std::make_unique<web_renderer>(FLAGS_web_bind, FLAGS_web_port, FLAGS_font_size);
#else
    throw std::runtime_error("--renderer=web requested but this binary was built with WISH_ENABLE_WEB=OFF");
#endif
  }
  throw std::runtime_error("unknown --renderer value '" + FLAGS_renderer + "' (expected sdl3 or web)");
}

example_client::example_client(
    bison::rmi::transport::memory_client_transport t,
    wish::renderer* renderer,
    bool verbose,
    std::string theme,
    std::string tag)
    : wish::client(std::move(t)),
      renderer_(renderer),
      verbose_(verbose),
      theme_(std::move(theme)),
      tag_(std::move(tag)) {}

void example_client::vlog(const std::string& msg) const {
  if (verbose_)
    std::clog << "[" << tag_ << "] " << msg << "\n";
}

int run_example(
    int argc,
    char* argv[],
    const char* tag,
    const std::string& window_title,
    int width,
    int height,
    bool allow_absolute_paths,
    const client_factory& make_client) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_verbose)
    std::clog << "[" << tag << "] starting\n";

  bison::rmi::transport::memory_server_transport transport;

  auto r = make_renderer(window_title, width, height);
  auto rptr = r.get();

  wish::server server{transport, std::move(r)};
  if (allow_absolute_paths)
    server.set_allow_absolute_paths(true);
  // Same-process examples are trusted local dev tooling (same trust level as
  // set_allow_absolute_paths above), so URL-sourced Image::src/font_path
  // fetches are safe to allow here too.
  server.set_allow_url_fetch(true);
  server.start(); // spawns render thread (SDL lives there) + bison listen thread

  if (FLAGS_verbose)
    std::clog << "[" << tag << "] server started -- connecting client\n";

  // run() blocks in on_session() until should_quit() goes true (window closed).
  auto client = make_client(transport.connect(), rptr, FLAGS_verbose, FLAGS_theme);
  client->run();

  if (FLAGS_verbose)
    std::clog << "[" << tag << "] client done -- stopping server\n";

  server.stop();
  return 0;
}

} // namespace bdg::wish::examples
