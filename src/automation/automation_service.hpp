// MIT License © 2025 Binary Dice Games
/// @file automation_service.hpp
/// @brief Per-session RMI service exposing native (ABI-driven) automation:
///        tree/hit-test queries, screenshots, input injection, and log access.
#pragma once

#ifdef WISH_AUTOMATION_ENABLED

#include <automation/automation_backend.hpp>
#include <context/logger.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace bdg::wish {

class automation_service;
using automation_service_ptr = std::shared_ptr<automation_service>;

/**
 * @brief Per-session service that exposes a renderer's `automation::automation_backend`
 *        over RMI, so a `wish::client` connection can drive/introspect the
 *        session's UI without a browser -- see `src/automation/DESIGN.md`'s
 *        "Native (ABI-based) automation" section.
 *
 * Registered in the `"wish"` bison namespace as `"__WishAutomation"`. Unlike
 * `style_service`/`logger` (always present), a session only gets one when
 * the active renderer implements `automation::automation_backend` (currently
 * only `sdl3_renderer`) -- `server::on_session_created`/
 * `standalone::on_session_created` check `renderer::as_automation_backend()`
 * before instantiating this. A client connecting to a server with no
 * automation-capable renderer never resolves `"__WishAutomation"` --
 * `context::find_singleton_service()` throws for this specific class when
 * unset, which `wish::client::on_connect()` catches non-fatally (see
 * `src/client/client.cpp`), so plain UI usage is unaffected.
 *
 * ## RMI methods exposed to clients
 *
 * | Method         | Params                              | Effect                                          |
 * |----------------|--------------------------------------|--------------------------------------------------|
 * | `get_tree`     | `"root"`: string (optional)          | Returns `{"json": "..."}` -- a TREE_SNAPSHOT-shaped payload (see `automation::build_tree_snapshot`) |
 * | `get_logs`     | --                                    | Returns `{"json": "..."}` -- a LOG_EVENT-shaped payload (see `automation::build_log_event`) |
 * | `screenshot`   | --                                    | Returns `{"data": "..."}` -- PNG-encoded bytes    |
 * | `mouse_move`   | `"x"`, `"y"`: float                  | Injects a synthetic mouse-move event              |
 * | `mouse_button` | `"button"`: int32, `"down"`: bool    | Injects a synthetic mouse-button press/release    |
 * | `key_event`    | `"keycode"`: int32, `"down"`: bool   | Injects a synthetic key press/release             |
 * | `text_input`   | `"utf8"`: string                     | Injects synthetic text input                      |
 *
 * `get_tree` and `screenshot` block the calling RMI dispatch thread until the
 * render thread services the request (see `automation::automation_backend`'s
 * doc comment); the four injection methods return immediately.
 */
class automation_service : public bison::dynamic {
 public:
  /**
   * @brief Construct and register RMI methods.
   * @param base     Prototype-initialised dynamic base (from `dynamic::instantiate`).
   * @param backend  Renderer-owned automation backend; not owned, must outlive this object.
   * @param logger   Session's logger service, for `get_logs`; may be null.
   */
  automation_service(bison::dynamic&& base, automation::automation_backend* backend, logger_ptr logger);

  /**
   * @brief Construct and register RMI methods.
   * @param backend  Renderer-owned automation backend; not owned, must outlive the result.
   * @param logger   Session's logger service, for `get_logs`; may be null.
   */
  static automation_service_ptr instantiate(automation::automation_backend* backend, logger_ptr logger);

 private:
  automation::automation_backend* backend_;
  logger_ptr logger_;
  std::atomic<uint32_t> next_request_id_{1};
};

/// @brief Register `"__WishAutomation"` in the `"wish"` bison class namespace.
void register_automation_service();

} // namespace bdg::wish

#endif // WISH_AUTOMATION_ENABLED
