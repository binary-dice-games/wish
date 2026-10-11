// MIT License © 2026 Binary Dice Games
/// @file conf.cpp
/// @brief Client-side runner for the conf embedded app.
#include "modules/bdg/desktop/conf/client/conf.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace bdg::wish {

using namespace bison;

/// Instantiate the server-side Conf form and ask it to re-read the stores
/// every two seconds, so settings other apps declare or change show up
/// while it is open. Returns when the user closes the window.
void run_conf(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Conf"_key).get());
  auto stop = std::make_shared<std::atomic<bool>>(false);

  proxy->onEvent("closed"_key, [&s, stop](dynamic) {
    stop->store(true, std::memory_order_relaxed);
    s.signal_done();
  });

  // `proxy` and `stop` stay alive through the shared_ptrs captured here,
  // like top's sampling thread.
  std::thread([proxy, stop] {
    using namespace std::chrono_literals;
    while (!stop->load(std::memory_order_relaxed)) {
      for (int i = 0; i < 20 && !stop->load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(100ms);
      if (stop->load(std::memory_order_relaxed))
        break;
      try {
        proxy->call("refresh"_key, dynamic{}).get();
      } catch (const std::exception&) {
        break; // form torn down (window closed / session ending)
      }
    }
  }).detach();
}

namespace {
struct conf_app_registrar {
  conf_app_registrar() {
    register_app({
        .name = "conf",
        .organization = WISH_MODULE_BDG_DESKTOP_CONF_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_CONF_COLLECTION,
        .description = "Settings: edit the settings of your apps and of the server, like macOS System Settings",
        .icon = "res/icons/settings.png",
        .params = {},
        .run = run_conf,
    });
  }
};
const conf_app_registrar conf_app_registrar_instance;
} // namespace

} // namespace bdg::wish
