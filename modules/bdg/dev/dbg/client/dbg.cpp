// MIT License © 2026 Binary Dice Games
/// @file dbg.cpp
/// @brief Client-side runner for the dbg (DebuggerFrontend) embedded app.
///
/// `wish client --run=dbg` -- instantiates the DebuggerFrontend form and
/// wires its `*_requested` events (see server/dbg.hpp) to a dbg_source
/// backed by a `debug_backend` chosen at launch time:
///   - default ("native"): the platform's own engine --
///     `win32_debug_backend` (Win32 debug API + DbgHelp) on Windows,
///     `posix_debug_backend` (drives a child `gdb`/`lldb-mi` over MI)
///     elsewhere. Debugs native/compiled processes.
///   - "python": `python_debug_backend`, which drives Microsoft's `debugpy`
///     over the Debug Adapter Protocol to debug a **Python** process.
///
/// The backend is selected via module arguments after `--`:
///   wish client --run=dbg -- --backend python [--connect host:port]
/// (the repo-wide convention for every module's own arguments -- see
/// `wish_app_host::app_args()`). All implement the same `debug_backend`
/// seam, so the event wiring below is identical regardless of which is used.
#include "dbg.hpp"
#include "dbg_source.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "python_debug_backend.hpp"
#if defined(_WIN32)
#include "win32_debug_backend.hpp"
#else
#include "posix_debug_backend.hpp"
#endif

namespace bdg::wish {

using namespace bison;

namespace {

/// @brief Parsed `wish client --run=dbg -- ...` module arguments.
struct dbg_options {
  std::string backend; ///< "", "native", or "python" (aliases folded in).
  std::string connect; ///< python connect-mode endpoint ("host:port").
};

dbg_options parse_dbg_args(const std::vector<std::string>& args) {
  dbg_options opt;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto value_of = [&](const std::string& flag) -> std::optional<std::string> {
      if (a == flag && i + 1 < args.size())
        return args[++i];
      if (a.rfind(flag + "=", 0) == 0)
        return a.substr(flag.size() + 1);
      return std::nullopt;
    };
    if (auto v = value_of("--backend"))
      opt.backend = *v;
    else if (auto v = value_of("--connect"))
      opt.connect = *v;
  }
  return opt;
}

std::unique_ptr<dbg::debug_backend> make_backend(const dbg_options& opt) {
  std::string b = opt.backend;
  std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return std::tolower(c); });

  if (b == "python" || b == "py" || b == "debugpy" || b == "pdb")
    return std::make_unique<dbg::python_debug_backend>(opt.connect);

  if (!b.empty() && b != "native" && b != "gdb" && b != "lldb" && b != "win32")
    std::cerr << "[dbg] unknown --backend '" << opt.backend << "', using the native backend\n";

#if defined(_WIN32)
  return std::make_unique<dbg::win32_debug_backend>();
#else
  return std::make_unique<dbg::posix_debug_backend>();
#endif
}

} // namespace

void run_dbg(wish_app_host& s) {
  auto proxy = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "DebuggerFrontend"_key).get());

  auto backend = make_backend(parse_dbg_args(s.app_args()));
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
        .params = {{"--backend <native|python>",
                    "Debug engine: 'native' (default; gdb/lldb-mi or the Win32 debug API) "
                    "or 'python' (debugpy over the Debug Adapter Protocol)"},
                   {"--connect <host:port>",
                    "python backend only: attach to a debugpy server the target already "
                    "started with debugpy.listen((host, port)), instead of injecting by PID"}},
        .run = run_dbg,
    });
  }
};
const dbg_app_registrar dbg_app_registrar_instance;
} // namespace

} // namespace bdg::wish
