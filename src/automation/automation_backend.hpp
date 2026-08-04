// MIT License © 2025 Binary Dice Games
/// @file automation_backend.hpp
/// @brief Renderer-side automation primitives, implemented by renderer
///        backends that support native (ABI-driven) automation.
#pragma once

#ifdef WISH_AUTOMATION_ENABLED

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace bdg::wish::automation {

/**
 * @brief Renderer-thread operations needed to service `automation_service`'s
 *        RMI calls.
 *
 * Implemented by renderer backends that can support automation without a
 * browser (currently `sdl3_renderer` -- see `src/automation/DESIGN.md`'s
 * "Native (ABI-based) automation" section). A `renderer` subclass that
 * implements this interface exposes it via `renderer::as_automation_backend()`;
 * `automation_service` forwards every RMI method it exposes to whichever
 * backend the active renderer returns (or the C ABI layer answers
 * `WISH_ERR_NOT_FOUND` if none does).
 *
 * `query_tree()` and `capture_screenshot()` need render-thread-owned state
 * (the current frame's hit-test map / pixel buffer), so they return a future
 * that resolves once the render thread has serviced the request -- the
 * calling RMI dispatch thread blocks on `.get()`. The four `inject_*`
 * methods are synchronous: they hand off to the platform's thread-safe
 * event queue (e.g. `SDL_PushEvent()`) and return immediately, exactly as
 * if the input had come from real hardware.
 */
class automation_backend {
 public:
  virtual ~automation_backend() = default;

  /**
   * @brief Request a tree/hit-test snapshot for the next frame the renderer
   *        completes.
   * @param request_id  Opaque id echoed back in the JSON reply (matches
   *                     `automation::build_tree_snapshot()`'s parameter).
   * @param root         Dot-path filter; empty means the whole tree.
   * @return Future resolving to the JSON text `build_tree_snapshot()` produces.
   */
  virtual std::future<std::string> query_tree(uint32_t request_id, const std::string& root) = 0;

  /**
   * @brief Request a screenshot of the next frame the renderer completes.
   * @return Future resolving to PNG-encoded image bytes.
   */
  virtual std::future<std::vector<uint8_t>> capture_screenshot() = 0;

  /// @brief Inject a synthetic mouse-move event at window-relative coordinates.
  virtual void inject_mouse_move(float x, float y) = 0;

  /// @brief Inject a synthetic mouse-button press/release.
  /// @param button  0 = left, 1 = right, 2 = middle (mirrors SDL's convention).
  virtual void inject_mouse_button(int button, bool down) = 0;

  /// @brief Inject a synthetic key press/release.
  /// @param keycode  Platform keycode (`SDL_Keycode` for `sdl3_renderer`).
  virtual void inject_key(int keycode, bool down) = 0;

  /// @brief Inject synthetic text input (e.g. for typing into an InputText).
  /// @param utf8  UTF-8 encoded text.
  virtual void inject_text(const std::string& utf8) = 0;
};

} // namespace bdg::wish::automation

#endif // WISH_AUTOMATION_ENABLED
