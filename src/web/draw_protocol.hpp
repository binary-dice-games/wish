// MIT License © 2025 Binary Dice Games
/// @file draw_protocol.hpp
/// @brief Wire format for streaming ImGui draw data / textures to the
///        browser client and decoding input events sent back.
///
/// wish owns this protocol outright (see `src/web/DESIGN.md`) rather than
/// depending on a third-party ImGui-over-network format, so there is no
/// version coupling to any other ImGui fork. Pure serialization only — no
/// networking dependency, which is what makes it unit-testable in
/// isolation (see `tests/test_web_renderer.cpp`).
///
/// All multi-byte integers/floats are little-endian, matching JS
/// `DataView` defaults and x86/ARM host byte order, so no byteswapping is
/// needed on either side.
///
/// Every WebSocket binary message is one envelope:
///   uint8    msg_type
///   uint8[3] reserved (zero)
///   uint32   payload_len
///   byte[payload_len] payload
///
/// See the `.cpp` for the exact per-message payload layouts.
#pragma once

#ifdef WISH_WEB_ENABLED

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace bdg::wish {

/// @brief Message type tags for the envelope's `msg_type` byte.
enum class web_msg_type : uint8_t {
  frame = 0x01,          ///< Server -> browser: one ImDrawData snapshot.
  tex_create = 0x02,     ///< Server -> browser: full texture upload.
  tex_update = 0x03,     ///< Server -> browser: partial texture upload.
  tex_destroy = 0x04,    ///< Server -> browser: drop a texture.
  tex_check = 0x05,      ///< Server -> browser: "do you have this cached?"
  input = 0x10,          ///< Browser -> server: one input event.
  resize = 0x11,         ///< Browser -> server: canvas size changed.
  cache_response = 0x12, ///< Browser -> server: hit/miss reply to TEX_CHECK.
  clipboard_text = 0x13, ///< Browser -> server: OS clipboard text, sent
                         ///< proactively before a paste keystroke (see
                         ///< "Clipboard bridging" in src/web/DESIGN.md).
  clipboard_write = 0x14, ///< Server -> browser: ImGui copied/cut text;
                          ///< browser should write it to the OS clipboard.
#ifdef WISH_AUTOMATION_ENABLED
  query_tree = 0x20,    ///< Browser -> server: request a tree/hit-test snapshot.
  tree_snapshot = 0x21, ///< Server -> browser: JSON tree/hit-test snapshot.
  log_event = 0x22,     ///< Server -> browser: newly-logged entries, pushed live.
#endif
};

/// @brief Discriminator for `web_input_event::kind`.
enum class web_input_kind : uint8_t {
  mouse_move = 0,
  mouse_button = 1,
  mouse_wheel = 2,
  key = 3,
  char_input = 4,
  touch_down = 5,   ///< A new finger touched the canvas.
  touch_move = 6,   ///< An already-down finger moved.
  touch_up = 7,     ///< A finger was lifted.
  touch_cancel = 8, ///< The browser canceled a touch (e.g. a system gesture
                    ///< took over) -- treated like `touch_up` by consumers.
  motion = 9,       ///< A `devicemotion` (accelerometer) sample.
};

/**
 * @brief One decoded browser -> server input event (INPUT payload).
 *
 * Not a tagged union on purpose (keeps encode/decode trivial and the type
 * cheaply copyable); only the fields relevant to `kind` are meaningful.
 */
struct web_input_event {
  web_input_kind kind{};
  float x = 0.0f;         ///< mouse_move, mouse_button, touch_down/move/up/cancel
  float y = 0.0f;         ///< mouse_move, mouse_button, touch_down/move/up/cancel
  uint8_t button = 0;     ///< mouse_button
  bool down = false;      ///< mouse_button, key
  float wheel_x = 0.0f;   ///< mouse_wheel
  float wheel_y = 0.0f;   ///< mouse_wheel
  uint32_t key_code = 0;  ///< key — ImGuiKey enum value
  uint32_t codepoint = 0; ///< char_input — UTF-32 codepoint
  uint32_t touch_id = 0;  ///< touch_down/move/up/cancel — the browser's
                          ///< `Touch.identifier`, stable for one finger's
                          ///< whole down-move*-up/cancel lifetime.
  float accel_x = 0.0f;   ///< motion — `DeviceMotionEvent.acceleration.x` (m/s^2).
  float accel_y = 0.0f;   ///< motion — `DeviceMotionEvent.acceleration.y` (m/s^2).
  float accel_z = 0.0f;   ///< motion — `DeviceMotionEvent.acceleration.z` (m/s^2).
};

/// @brief One decoded browser -> server canvas resize (RESIZE payload).
struct web_resize_event {
  float width = 0.0f;
  float height = 0.0f;
  float dpr = 1.0f; ///< devicePixelRatio; feeds ImDrawData::FramebufferScale.
};

/**
 * @brief One decoded browser -> server reply to a TEX_CHECK (CACHE_RESPONSE
 *        payload): whether the browser already had this texture cached.
 */
struct web_cache_response {
  uint32_t texture_id = 0;
  bool hit = false;
};

namespace draw_protocol {

/**
 * @brief Encode one ImDrawData snapshot as a FRAME message.
 *
 * @param target_id  `0` (default) means "the visible canvas" -- today's only
 *                    behavior. A non-zero id means "render this draw data
 *                    into the offscreen render target with this id instead
 *                    of the canvas" -- see `imgui_renderer::begin_render_
 *                    target()`/`flush_draw_list()` and `web_renderer`'s
 *                    override of them. The id is drawn from the same space
 *                    as texture ids (assigned via `web_renderer`'s
 *                    `next_texture_id_` counter), since a render target's
 *                    color attachment is later sampled like any other
 *                    texture by a normal draw command (compositing it into
 *                    the canvas frame).
 */
std::vector<std::byte> encode_frame(const ImDrawData& draw_data, uint32_t target_id = 0);

/**
 * @brief Encode a texture (re)upload as a TEX_CREATE or TEX_UPDATE message.
 *
 * The message type is chosen from `tex.Status`: `ImTextureStatus_WantCreate`
 * encodes the whole texture as one rect (TEX_CREATE); any other status
 * (normally `ImTextureStatus_WantUpdates`) encodes only `tex.Updates[]`
 * (TEX_UPDATE).
 *
 * @param texture_id  wish-assigned id for this texture (NOT `tex.TexID`,
 *                     which the caller is responsible for setting via
 *                     `tex.SetTexID()` once the upload has been sent).
 */
std::vector<std::byte> encode_texture_update(uint32_t texture_id, const ImTextureData& tex);

/// @brief Encode a TEX_DESTROY message for a previously-uploaded texture.
std::vector<std::byte> encode_texture_destroy(uint32_t texture_id);

/**
 * @brief Encode a TEX_CHECK message: "does the browser already have this
 *        exact (path, crc32) cached?" -- metadata only, no pixel payload.
 *
 * Sent instead of a full TEX_CREATE for a cacheable texture during the
 * `pending_sync_` per-connection resync path (see `web_renderer::end_frame()`
 * and `src/web/DESIGN.md`); never on the live per-frame broadcast.
 *
 * @param texture_id  wish-assigned id for this texture (matches what a
 *                     follow-up TEX_CREATE for the same texture would use).
 * @param path        Path relative to the session resource directory,
 *                     identifying the resource independent of `texture_id`
 *                     (which is only stable within one server process run).
 * @param crc32       Content-version number (see `web_renderer::texture_meta`).
 * @param tex         Source texture; only `Format`/`Width`/`Height` are read.
 */
std::vector<std::byte> encode_texture_check(
    uint32_t texture_id, const std::string& path, uint32_t crc32, const ImTextureData& tex);

/**
 * @brief Decode one browser -> server WebSocket binary message as an INPUT
 *        event.
 *
 * @param message  The full envelope-wrapped message bytes as received from
 *                  the socket.
 * @return `std::nullopt` if `message` isn't a well-formed INPUT message
 *         (wrong `msg_type`, truncated payload, or unknown `event_kind`).
 */
std::optional<web_input_event> decode_input_message(std::span<const std::byte> message);

/**
 * @brief Decode one browser -> server WebSocket binary message as a RESIZE
 *        event.
 *
 * @param message  The full envelope-wrapped message bytes as received from
 *                  the socket.
 * @return `std::nullopt` if `message` isn't a well-formed RESIZE message.
 */
std::optional<web_resize_event> decode_resize_message(std::span<const std::byte> message);

/**
 * @brief Decode one browser -> server WebSocket binary message as a
 *        CACHE_RESPONSE (the reply to a TEX_CHECK).
 *
 * @param message  The full envelope-wrapped message bytes as received from
 *                  the socket.
 * @return `std::nullopt` if `message` isn't a well-formed CACHE_RESPONSE
 *         message.
 */
std::optional<web_cache_response> decode_cache_response_message(std::span<const std::byte> message);

/**
 * @brief Decode one browser -> server WebSocket binary message as a
 *        CLIPBOARD_TEXT payload: the browser's current OS clipboard text.
 *
 * Sent proactively by the browser just before the key event for a Ctrl+V
 * (see "Clipboard bridging" in src/web/DESIGN.md) -- `navigator.clipboard
 * .readText()` is async, so the text must land server-side and be cached
 * *before* ImGui processes the paste keystroke, since `GetClipboardTextFn`
 * is a synchronous callback with no way to await a round trip itself.
 *
 * @param message  The full envelope-wrapped message bytes as received from
 *                  the socket.
 * @return `std::nullopt` if @p message isn't a well-formed CLIPBOARD_TEXT
 *         message; otherwise the payload bytes decoded as UTF-8 text.
 */
std::optional<std::string> decode_clipboard_text_message(std::span<const std::byte> message);

/**
 * @brief Encode a CLIPBOARD_WRITE message: text ImGui just copied/cut
 *        server-side, for the browser to push to the real OS clipboard via
 *        `navigator.clipboard.writeText()`.
 *
 * @param text  UTF-8 text, as produced by ImGui's `SetClipboardTextFn`
 *              callback.
 */
std::vector<std::byte> encode_clipboard_write(const std::string& text);

#ifdef WISH_AUTOMATION_ENABLED
/**
 * @brief Decode one browser -> server QUERY_TREE message as its raw UTF-8
 *        JSON payload -- unparsed.
 *
 * Unlike the other decode_*_message() functions, this does not parse the
 * payload into a struct: `draw_protocol` has no `nlohmann::json` dependency
 * (kept a pure binary-envelope codec, see the file doc comment), so parsing
 * the `{"request_id":N,"root":"..."}` body is left to
 * `automation::parse_query_tree_request()` (src/automation/automation_query.hpp),
 * which already depends on `nlohmann::json` for building the reply.
 *
 * @param message  The full envelope-wrapped message bytes as received from
 *                  the socket.
 * @return `std::nullopt` if `message` isn't a well-formed QUERY_TREE message
 *         (wrong `msg_type` or truncated envelope); otherwise the payload
 *         bytes decoded as UTF-8 text, valid JSON or not.
 */
std::optional<std::string> decode_query_tree_message(std::span<const std::byte> message);

/**
 * @brief Encode a TREE_SNAPSHOT message wrapping an already-serialized JSON
 *        payload.
 *
 * @param json_payload  UTF-8 JSON text, e.g. `automation::build_tree_snapshot()`'s
 *                       return value. Copied verbatim into the envelope
 *                       payload -- this function does not itself touch JSON.
 */
std::vector<std::byte> encode_tree_snapshot(const std::string& json_payload);

/**
 * @brief Encode a LOG_EVENT message wrapping an already-serialized JSON
 *        payload -- mirrors `encode_tree_snapshot()`.
 *
 * Unlike TREE_SNAPSHOT (a reply to a browser-initiated QUERY_TREE),
 * LOG_EVENT is pushed to every connected browser as soon as new log
 * entries exist -- there is no corresponding browser -> server request
 * message. See `automation::build_log_event()` and
 * `web_renderer::service_automation_queries()`.
 *
 * @param json_payload  UTF-8 JSON text, e.g. `automation::build_log_event()`'s
 *                       return value.
 */
std::vector<std::byte> encode_log_event(const std::string& json_payload);
#endif

} // namespace draw_protocol
} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
