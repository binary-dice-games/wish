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
#include <vector>

struct ImDrawData;
struct ImTextureData;

namespace bdg::wish {

/// @brief Message type tags for the envelope's `msg_type` byte.
enum class web_msg_type : uint8_t {
  frame = 0x01,       ///< Server -> browser: one ImDrawData snapshot.
  tex_create = 0x02,  ///< Server -> browser: full texture upload.
  tex_update = 0x03,  ///< Server -> browser: partial texture upload.
  tex_destroy = 0x04, ///< Server -> browser: drop a texture.
  input = 0x10,       ///< Browser -> server: one input event.
  resize = 0x11,      ///< Browser -> server: canvas size changed.
};

/// @brief Discriminator for `web_input_event::kind`.
enum class web_input_kind : uint8_t {
  mouse_move = 0,
  mouse_button = 1,
  mouse_wheel = 2,
  key = 3,
  char_input = 4,
};

/**
 * @brief One decoded browser -> server input event (INPUT payload).
 *
 * Not a tagged union on purpose (keeps encode/decode trivial and the type
 * cheaply copyable); only the fields relevant to `kind` are meaningful.
 */
struct web_input_event {
  web_input_kind kind{};
  float x = 0.0f;         ///< mouse_move, mouse_button
  float y = 0.0f;         ///< mouse_move, mouse_button
  uint8_t button = 0;     ///< mouse_button
  bool down = false;      ///< mouse_button, key
  float wheel_x = 0.0f;   ///< mouse_wheel
  float wheel_y = 0.0f;   ///< mouse_wheel
  uint32_t key_code = 0;  ///< key — ImGuiKey enum value
  uint32_t codepoint = 0; ///< char_input — UTF-32 codepoint
};

/// @brief One decoded browser -> server canvas resize (RESIZE payload).
struct web_resize_event {
  float width = 0.0f;
  float height = 0.0f;
  float dpr = 1.0f; ///< devicePixelRatio; feeds ImDrawData::FramebufferScale.
};

namespace draw_protocol {

/// @brief Encode one ImDrawData snapshot as a FRAME message.
std::vector<std::byte> encode_frame(const ImDrawData& draw_data);

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

} // namespace draw_protocol
} // namespace bdg::wish

#endif // WISH_WEB_ENABLED
