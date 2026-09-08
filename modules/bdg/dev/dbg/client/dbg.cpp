// MIT License © 2026 Binary Dice Games
/// @file dbg.cpp
/// @brief Client-side runner for the dbg (DebuggerFrontend) embedded app.
///
/// `wish client --run=dbg` -- instantiates the DebuggerFrontend form and
/// wires its `*_requested` events (see server/dbg.hpp) to a dbg_source
/// backed by a platform debug_backend:
///   - Windows: `win32_debug_backend` (Win32 debug API + DbgHelp).
///   - Linux/macOS: `posix_debug_backend` (drives a child `gdb`/`lldb-mi`
///     over its MI protocol).
/// Both implement the same `debug_backend` seam, so the event wiring below
/// is identical regardless of which one is in use.
#include "dbg.hpp"
#include "dbg_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <memory>

#if defined(_WIN32)
#include "win32_debug_backend.hpp"
#else
#include "posix_debug_backend.hpp"
#endif

namespace bdg::wish {

using namespace bison;

void run_dbg(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "DebuggerFrontend"_key).get());

#if defined(_WIN32)
  auto backend = std::make_unique<dbg::win32_debug_backend>();
#else
  auto backend = std::make_unique<dbg::posix_debug_backend>();
#endif
  auto source = std::make_shared<dbg::dbg_source>(proxy, std::move(backend));

  proxy->onEvent("attach_requested"_key,
                 [source](dynamic payload) { source->on_attach_requested(payload.as<uint32_t>("pid"_key)); });
  proxy->onEvent("detach_requested"_key, [source](dynamic) { source->on_detach_requested(); });
  proxy->onEvent("pause_requested"_key, [source](dynamic) { source->on_pause_requested(); });
  proxy->onEvent("resume_requested"_key, [source](dynamic) { source->on_resume_requested(); });
  proxy->onEvent("step_requested"_key, [source](dynamic payload) {
    source->on_step_requested(payload.as<std::string>("kind"_key), payload.as<uint32_t>("thread_id"_key));
  });
  proxy->onEvent("toggle_breakpoint_requested"_key, [source](dynamic payload) {
    source->on_toggle_breakpoint_requested(payload.as<std::string>("path"_key), payload.as<int32_t>("line"_key));
  });
  proxy->onEvent("select_thread_requested"_key, [source](dynamic payload) {
    source->on_select_thread_requested(payload.as<uint32_t>("thread_id"_key));
  });
  proxy->onEvent("select_frame_requested"_key, [source](dynamic payload) {
    source->on_select_frame_requested(payload.as<uint32_t>("frame_id"_key));
  });
  proxy->onEvent("add_watch_requested"_key,
                 [source](dynamic payload) { source->on_add_watch_requested(payload.as<std::string>("expr"_key)); });
  proxy->onEvent("open_file_requested"_key, [source](dynamic payload) {
    source->on_open_file_requested(payload.as<std::string>("path"_key), payload.as<int32_t>("line"_key));
  });

  proxy->onEvent("closed"_key, [&s, source](dynamic) { s.signal_done(); });
}

namespace {
struct dbg_app_registrar {
  dbg_app_registrar() {
    register_app({
        .name = "dbg",
        .organization = WISH_MODULE_BDG_DEV_DBG_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DEV_DBG_COLLECTION,
        .description = "Visual-Studio-style debugger GUI frontend (wish client --run=dbg)",
        .params = {},
        .run = run_dbg,
    });
  }
};
const dbg_app_registrar dbg_app_registrar_instance;
} // namespace

} // namespace bdg::wish
