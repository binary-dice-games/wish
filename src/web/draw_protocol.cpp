// MIT License © 2025 Binary Dice Games
/// @file draw_protocol.cpp
/// @brief Implementation of bdg::wish::draw_protocol.
#include <web/draw_protocol.hpp>

#ifdef WISH_WEB_ENABLED

#include <imgui.h>

#include <cstring>

namespace bdg::wish::draw_protocol {

// wish's build never overrides ImDrawIdx (see imgui.h); the wire format
// hard-codes a 16-bit index width. If this config ever changes, fail loudly
// here instead of silently corrupting frames on the wire.
static_assert(sizeof(ImDrawIdx) == 2, "draw_protocol wire format assumes a 16-bit ImDrawIdx");

// ── byte-buffer writer helpers ──────────────────────────────────────────────

namespace {

void put_u8(std::vector<std::byte>& buf, uint8_t v) {
  buf.push_back(std::byte{v});
}

void put_u32(std::vector<std::byte>& buf, uint32_t v) {
  std::byte b[4];
  std::memcpy(b, &v, sizeof(v));
  buf.insert(buf.end(), b, b + sizeof(b));
}

void put_f32(std::vector<std::byte>& buf, float v) {
  uint32_t u;
  std::memcpy(&u, &v, sizeof(u));
  put_u32(buf, u);
}

void put_bytes(std::vector<std::byte>& buf, const void* data, size_t n) {
  auto p = static_cast<const std::byte*>(data);
  buf.insert(buf.end(), p, p + n);
}

// Prepend the envelope header (msg_type + 3 reserved zero bytes + payload
// length) to an already-built payload.
std::vector<std::byte> wrap_envelope(web_msg_type type, std::vector<std::byte> payload) {
  std::vector<std::byte> out;
  out.reserve(payload.size() + 8);
  put_u8(out, static_cast<uint8_t>(type));
  put_u8(out, 0);
  put_u8(out, 0);
  put_u8(out, 0);
  put_u32(out, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// ── byte-buffer reader helper ───────────────────────────────────────────────

class reader {
 public:
  explicit reader(std::span<const std::byte> data) : data_(data) {}

  bool has(size_t n) const {
    return pos_ + n <= data_.size();
  }

  std::optional<uint8_t> u8() {
    if (!has(1))
      return std::nullopt;
    uint8_t v = std::to_integer<uint8_t>(data_[pos_]);
    pos_ += 1;
    return v;
  }

  std::optional<uint32_t> u32() {
    if (!has(4))
      return std::nullopt;
    uint32_t v;
    std::memcpy(&v, data_.data() + pos_, sizeof(v));
    pos_ += 4;
    return v;
  }

  std::optional<float> f32() {
    auto u = u32();
    if (!u)
      return std::nullopt;
    float v;
    std::memcpy(&v, &*u, sizeof(v));
    return v;
  }

 private:
  std::span<const std::byte> data_;
  size_t pos_ = 0;
};

// Validate + strip the envelope, returning the payload span if `msg_type`
// matches @p expected. `std::nullopt` on any mismatch or truncation.
std::optional<std::span<const std::byte>> unwrap_envelope(std::span<const std::byte> message, web_msg_type expected) {
  reader r{message};
  auto type = r.u8();
  if (!type || static_cast<web_msg_type>(*type) != expected)
    return std::nullopt;
  // 3 reserved bytes.
  if (!r.u8() || !r.u8() || !r.u8())
    return std::nullopt;
  auto len = r.u32();
  if (!len)
    return std::nullopt;
  std::span<const std::byte> payload = message.subspan(8);
  if (payload.size() != *len)
    return std::nullopt;
  return payload;
}

} // namespace

// ── outbound: FRAME ─────────────────────────────────────────────────────────

std::vector<std::byte> encode_frame(const ImDrawData& draw_data) {
  std::vector<std::byte> payload;

  put_f32(payload, draw_data.DisplayPos.x);
  put_f32(payload, draw_data.DisplayPos.y);
  put_f32(payload, draw_data.DisplaySize.x);
  put_f32(payload, draw_data.DisplaySize.y);
  put_f32(payload, draw_data.FramebufferScale.x);
  put_f32(payload, draw_data.FramebufferScale.y);
  put_u32(payload, static_cast<uint32_t>(draw_data.TotalVtxCount));
  put_u32(payload, static_cast<uint32_t>(draw_data.TotalIdxCount));
  put_u32(payload, static_cast<uint32_t>(draw_data.CmdListsCount));

  for (int i = 0; i < draw_data.CmdListsCount; ++i) {
    const ImDrawList& cmd_list = *draw_data.CmdLists[i];

    put_u32(payload, static_cast<uint32_t>(cmd_list.VtxBuffer.Size));
    put_u32(payload, static_cast<uint32_t>(cmd_list.IdxBuffer.Size));
    put_bytes(payload, cmd_list.VtxBuffer.Data, static_cast<size_t>(cmd_list.VtxBuffer.Size) * sizeof(ImDrawVert));
    put_bytes(payload, cmd_list.IdxBuffer.Data, static_cast<size_t>(cmd_list.IdxBuffer.Size) * sizeof(ImDrawIdx));

    // ImDrawCmd::UserCallback entries (e.g. ImDrawCallback_ResetRenderState)
    // have no meaning to the browser's WebGL2 renderer and are dropped;
    // count only the real, renderable commands.
    uint32_t real_cmd_count = 0;
    for (const ImDrawCmd& cmd : cmd_list.CmdBuffer)
      if (!cmd.UserCallback)
        ++real_cmd_count;
    put_u32(payload, real_cmd_count);

    for (const ImDrawCmd& cmd : cmd_list.CmdBuffer) {
      if (cmd.UserCallback)
        continue;
      put_f32(payload, cmd.ClipRect.x);
      put_f32(payload, cmd.ClipRect.y);
      put_f32(payload, cmd.ClipRect.z);
      put_f32(payload, cmd.ClipRect.w);
      put_u32(payload, static_cast<uint32_t>(cmd.GetTexID()));
      put_u32(payload, cmd.VtxOffset);
      put_u32(payload, cmd.IdxOffset);
      put_u32(payload, cmd.ElemCount);
    }
  }

  return wrap_envelope(web_msg_type::frame, std::move(payload));
}

// ── outbound: TEX_CREATE / TEX_UPDATE ───────────────────────────────────────

std::vector<std::byte> encode_texture_update(uint32_t texture_id, const ImTextureData& tex) {
  std::vector<std::byte> payload;

  put_u32(payload, texture_id);
  put_u8(payload, tex.Format == ImTextureFormat_Alpha8 ? 1 : 0);
  put_u32(payload, static_cast<uint32_t>(tex.Width));
  put_u32(payload, static_cast<uint32_t>(tex.Height));

  const bool is_create = tex.Status == ImTextureStatus_WantCreate;
  if (is_create) {
    put_u32(payload, 1); // one rect covering the whole texture
    put_u32(payload, 0);
    put_u32(payload, 0);
    put_u32(payload, static_cast<uint32_t>(tex.Width));
    put_u32(payload, static_cast<uint32_t>(tex.Height));
    put_bytes(payload, tex.Pixels, static_cast<size_t>(tex.GetSizeInBytes()));
  } else {
    put_u32(payload, static_cast<uint32_t>(tex.Updates.Size));
    for (const ImTextureRect& rect : tex.Updates) {
      put_u32(payload, rect.x);
      put_u32(payload, rect.y);
      put_u32(payload, rect.w);
      put_u32(payload, rect.h);
      // Rows are contiguous within a rect but the rect may not span the
      // full texture width, so copy row-by-row using the atlas's pitch.
      for (int row = 0; row < rect.h; ++row) {
        const void* row_ptr = const_cast<ImTextureData&>(tex).GetPixelsAt(rect.x, rect.y + row);
        put_bytes(payload, row_ptr, static_cast<size_t>(rect.w) * tex.BytesPerPixel);
      }
    }
  }

  return wrap_envelope(is_create ? web_msg_type::tex_create : web_msg_type::tex_update, std::move(payload));
}

// ── outbound: TEX_DESTROY ───────────────────────────────────────────────────

std::vector<std::byte> encode_texture_destroy(uint32_t texture_id) {
  std::vector<std::byte> payload;
  put_u32(payload, texture_id);
  return wrap_envelope(web_msg_type::tex_destroy, std::move(payload));
}

// ── outbound: TEX_CHECK ─────────────────────────────────────────────────────

std::vector<std::byte> encode_texture_check(
    uint32_t texture_id, const std::string& path, uint32_t crc32, const ImTextureData& tex) {
  std::vector<std::byte> payload;

  put_u32(payload, texture_id);
  put_u8(payload, tex.Format == ImTextureFormat_Alpha8 ? 1 : 0);
  put_u32(payload, static_cast<uint32_t>(tex.Width));
  put_u32(payload, static_cast<uint32_t>(tex.Height));
  put_u32(payload, crc32);
  put_u32(payload, static_cast<uint32_t>(path.size()));
  put_bytes(payload, path.data(), path.size());

  return wrap_envelope(web_msg_type::tex_check, std::move(payload));
}

// ── inbound: INPUT ───────────────────────────────────────────────────────────

std::optional<web_input_event> decode_input_message(std::span<const std::byte> message) {
  auto payload = unwrap_envelope(message, web_msg_type::input);
  if (!payload)
    return std::nullopt;

  reader r{*payload};
  auto kind_byte = r.u8();
  if (!kind_byte)
    return std::nullopt;

  web_input_event ev;
  switch (static_cast<web_input_kind>(*kind_byte)) {
    case web_input_kind::mouse_move: {
      auto x = r.f32();
      auto y = r.f32();
      if (!x || !y)
        return std::nullopt;
      ev.kind = web_input_kind::mouse_move;
      ev.x = *x;
      ev.y = *y;
      return ev;
    }
    case web_input_kind::mouse_button: {
      auto button = r.u8();
      auto down = r.u8();
      auto x = r.f32();
      auto y = r.f32();
      if (!button || !down || !x || !y)
        return std::nullopt;
      ev.kind = web_input_kind::mouse_button;
      ev.button = *button;
      ev.down = *down != 0;
      ev.x = *x;
      ev.y = *y;
      return ev;
    }
    case web_input_kind::mouse_wheel: {
      auto dx = r.f32();
      auto dy = r.f32();
      if (!dx || !dy)
        return std::nullopt;
      ev.kind = web_input_kind::mouse_wheel;
      ev.wheel_x = *dx;
      ev.wheel_y = *dy;
      return ev;
    }
    case web_input_kind::key: {
      auto key_code = r.u32();
      auto down = r.u8();
      if (!key_code || !down)
        return std::nullopt;
      ev.kind = web_input_kind::key;
      ev.key_code = *key_code;
      ev.down = *down != 0;
      return ev;
    }
    case web_input_kind::char_input: {
      auto codepoint = r.u32();
      if (!codepoint)
        return std::nullopt;
      ev.kind = web_input_kind::char_input;
      ev.codepoint = *codepoint;
      return ev;
    }
    default:
      return std::nullopt;
  }
}

// ── inbound: RESIZE ──────────────────────────────────────────────────────────

std::optional<web_resize_event> decode_resize_message(std::span<const std::byte> message) {
  auto payload = unwrap_envelope(message, web_msg_type::resize);
  if (!payload)
    return std::nullopt;

  reader r{*payload};
  auto width = r.f32();
  auto height = r.f32();
  auto dpr = r.f32();
  if (!width || !height || !dpr)
    return std::nullopt;

  return web_resize_event{*width, *height, *dpr};
}

// ── inbound: CACHE_RESPONSE ──────────────────────────────────────────────────

std::optional<web_cache_response> decode_cache_response_message(std::span<const std::byte> message) {
  auto payload = unwrap_envelope(message, web_msg_type::cache_response);
  if (!payload)
    return std::nullopt;

  reader r{*payload};
  auto texture_id = r.u32();
  auto hit = r.u8();
  if (!texture_id || !hit)
    return std::nullopt;

  return web_cache_response{*texture_id, *hit != 0};
}

// ── inbound: CLIPBOARD_TEXT / outbound: CLIPBOARD_WRITE ─────────────────────

std::optional<std::string> decode_clipboard_text_message(std::span<const std::byte> message) {
  auto payload = unwrap_envelope(message, web_msg_type::clipboard_text);
  if (!payload)
    return std::nullopt;
  return std::string(reinterpret_cast<const char*>(payload->data()), payload->size());
}

std::vector<std::byte> encode_clipboard_write(const std::string& text) {
  std::vector<std::byte> payload(text.size());
  std::memcpy(payload.data(), text.data(), text.size());
  return wrap_envelope(web_msg_type::clipboard_write, std::move(payload));
}

// ── inbound: QUERY_TREE / outbound: TREE_SNAPSHOT ───────────────────────────

#ifdef WISH_AUTOMATION_ENABLED

std::optional<std::string> decode_query_tree_message(std::span<const std::byte> message) {
  auto payload = unwrap_envelope(message, web_msg_type::query_tree);
  if (!payload)
    return std::nullopt;
  return std::string(reinterpret_cast<const char*>(payload->data()), payload->size());
}

std::vector<std::byte> encode_tree_snapshot(const std::string& json_payload) {
  std::vector<std::byte> payload(json_payload.size());
  std::memcpy(payload.data(), json_payload.data(), json_payload.size());
  return wrap_envelope(web_msg_type::tree_snapshot, std::move(payload));
}

// ── outbound: LOG_EVENT ──────────────────────────────────────────────────────

std::vector<std::byte> encode_log_event(const std::string& json_payload) {
  std::vector<std::byte> payload(json_payload.size());
  std::memcpy(payload.data(), json_payload.data(), json_payload.size());
  return wrap_envelope(web_msg_type::log_event, std::move(payload));
}

#endif // WISH_AUTOMATION_ENABLED

} // namespace bdg::wish::draw_protocol

#endif // WISH_WEB_ENABLED
