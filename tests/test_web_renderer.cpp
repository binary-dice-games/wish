// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <web/civetweb_server.hpp>
#include <web/draw_protocol.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using bdg::wish::web_input_event;
using bdg::wish::web_input_kind;
using bdg::wish::web_msg_type;
using bdg::wish::web_renderer;
using bdg::wish::web_resize_event;
using bdg::wish::draw_protocol::decode_input_message;
using bdg::wish::draw_protocol::decode_resize_message;
using bdg::wish::draw_protocol::encode_frame;
using bdg::wish::draw_protocol::encode_texture_destroy;
using bdg::wish::draw_protocol::encode_texture_update;

// ── byte-buffer helpers (test-local, deliberately independent of
//    draw_protocol.cpp's private reader/writer so tests catch drift against
//    the *documented* wire format, not just against the encoder's own
//    internal logic) ──────────────────────────────────────────────────────

namespace {

void push_u8(std::vector<std::byte>& buf, uint8_t v) {
  buf.push_back(std::byte{v});
}

void push_u32(std::vector<std::byte>& buf, uint32_t v) {
  std::byte b[4];
  std::memcpy(b, &v, sizeof(v));
  buf.insert(buf.end(), b, b + sizeof(b));
}

void push_f32(std::vector<std::byte>& buf, float v) {
  uint32_t u;
  std::memcpy(&u, &v, sizeof(u));
  push_u32(buf, u);
}

std::vector<std::byte> build_envelope(web_msg_type type, const std::vector<std::byte>& payload) {
  std::vector<std::byte> out;
  push_u8(out, static_cast<uint8_t>(type));
  push_u8(out, 0);
  push_u8(out, 0);
  push_u8(out, 0);
  push_u32(out, static_cast<uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

uint8_t read_u8(const std::vector<std::byte>& buf, size_t& pos) {
  return std::to_integer<uint8_t>(buf[pos++]);
}

uint32_t read_u32(const std::vector<std::byte>& buf, size_t& pos) {
  uint32_t v;
  std::memcpy(&v, buf.data() + pos, sizeof(v));
  pos += sizeof(v);
  return v;
}

float read_f32(const std::vector<std::byte>& buf, size_t& pos) {
  uint32_t u = read_u32(buf, pos);
  float v;
  std::memcpy(&v, &u, sizeof(v));
  return v;
}

// Hand-rolled WS client handshake -- returns a connected + upgraded socket,
// or -1 on failure. Doesn't validate Sec-WebSocket-Accept (only "101").
int connect_ws_client(int port) {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(sock);
    return -1;
  }

  std::string handshake =
      "GET /ws HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  ::send(sock, handshake.data(), handshake.size(), 0);

  std::string resp;
  char c;
  while (resp.size() < 8192 && ::recv(sock, &c, 1, 0) == 1) {
    resp.push_back(c);
    if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0)
      break;
  }
  if (resp.find("101") == std::string::npos) {
    ::close(sock);
    return -1;
  }
  return sock;
}

// Sends @p payload as one masked client->server binary WS frame (RFC6455
// requires client frames to be masked; civetweb unmasks them before handing
// the payload to on_message). Only handles payloads small enough for our
// tests' 7-bit length field (< 126 bytes).
void send_ws_binary(int sock, const std::vector<std::byte>& payload) {
  std::vector<unsigned char> frame;
  frame.push_back(0x82); // FIN + binary opcode
  frame.push_back(static_cast<unsigned char>(0x80 | payload.size())); // MASK bit + 7-bit length
  unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
  frame.insert(frame.end(), mask, mask + 4);
  for (size_t i = 0; i < payload.size(); ++i)
    frame.push_back(static_cast<unsigned char>(std::to_integer<uint8_t>(payload[i]) ^ mask[i % 4]));
  ::send(sock, reinterpret_cast<const char*>(frame.data()), frame.size(), 0);
}

} // namespace

// ── Test fixture ──────────────────────────────────────────────────────────────
//
// Unlike ImguiRendererTest, web_renderer::setup()/teardown() manage the
// ImGui context themselves, so the fixture doesn't create one.

class WebRendererTest : public ::testing::Test {
 protected:
  void SetUp() override {
    renderer_ = std::make_unique<web_renderer>("127.0.0.1", 0, 16);
    renderer_->setup();
  }

  void TearDown() override {
    renderer_->teardown();
  }

  // Renders one minimal real frame and returns its ImDrawData. Also assigns
  // texture ids for any texture in ImTextureStatus_WantCreate (the font
  // atlas, on the first frame) -- this previews what web_renderer::end_frame()
  // will do once texture broadcasting is wired (a later step): ImDrawCmd::GetTexID()
  // asserts its texture has a valid TexID, so encode_frame() cannot run
  // against a freshly-rendered frame until *something* has assigned one.
  const ImDrawData* render_test_frame() {
    renderer_->begin_frame();
    ImGui::Begin("test");
    ImGui::Button("OK");
    ImGui::End();
    renderer_->end_frame();

    const ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data && draw_data->Textures) {
      int next_id = 1;
      for (ImTextureData* tex : *draw_data->Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) {
          tex->SetTexID(static_cast<ImTextureID>(next_id));
          tex->SetStatus(ImTextureStatus_OK);
        }
        ++next_id;
      }
    }
    return draw_data;
  }

  std::unique_ptr<web_renderer> renderer_;
};

// ── should_quit() / request_quit() ──────────────────────────────────────────
//
// Regression guard: unlike sdl3_renderer, web_renderer must NOT auto-quit
// (e.g. on zero connected clients) — only an explicit request_quit() call
// may set should_quit() true. See src/web/DESIGN.md.

TEST_F(WebRendererTest, ShouldQuit_DefaultsFalse) {
  EXPECT_FALSE(renderer_->should_quit());
}

TEST_F(WebRendererTest, RequestQuit_SetsShouldQuitTrue) {
  renderer_->request_quit();
  EXPECT_TRUE(renderer_->should_quit());
}

// ── frame lifecycle ──────────────────────────────────────────────────────────

TEST_F(WebRendererTest, BeginEndFrame_DoesNotThrow) {
  EXPECT_NO_THROW({
    renderer_->begin_frame();
    renderer_->end_frame();
  });
}

// ── asset extraction ─────────────────────────────────────────────────────────

TEST_F(WebRendererTest, Setup_ExtractsWebAssetsAndServesIndexOverHttp) {
  const auto& dir = renderer_->web_assets_dir();
  ASSERT_FALSE(dir.empty());
  EXPECT_TRUE(std::filesystem::exists(dir / "web" / "index.html"));
  EXPECT_TRUE(std::filesystem::exists(dir / "web" / "client.js"));
}

TEST(WebRendererAssetTeardownTest, Teardown_RemovesExtractedAssetDirectory) {
  web_renderer renderer("127.0.0.1", 0, 16);
  renderer.setup();
  auto dir = renderer.web_assets_dir();
  ASSERT_TRUE(std::filesystem::exists(dir));
  renderer.teardown();
  EXPECT_FALSE(std::filesystem::exists(dir));
}

// ── inbound input handling ──────────────────────────────────────────────────
//
// End-to-end through a real WebSocket connection: this is the actual path
// browser input takes (on_message on a civetweb worker thread -> input_queue_
// -> begin_frame() drains it into ImGuiIO on the render thread), not just a
// direct call into a private member.

TEST_F(WebRendererTest, PollEvents_FalseThenTrueAfterConnect) {
  EXPECT_FALSE(renderer_->poll_events()); // no activity yet

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);

  bool activity = false;
  for (int i = 0; i < 200 && !activity; ++i) {
    activity = renderer_->poll_events();
    if (!activity)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(activity);

  ::close(sock);
}

TEST_F(WebRendererTest, BeginFrame_DrainsQueuedMouseMoveIntoImGuiIO) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);

  // connect_ws_client() itself already triggers on_connect (activity_ =
  // true) -- drain that first so the wait loop below reacts to the message
  // sent next, not to the earlier connect.
  for (int i = 0; i < 200; ++i) {
    if (renderer_->poll_events())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_move));
  push_f32(payload, 42.0f);
  push_f32(payload, 17.0f);
  send_ws_binary(sock, build_envelope(web_msg_type::input, payload));

  // Wait for the on_message callback (a civetweb worker thread) to push the
  // decoded event into input_queue_ and flip activity_.
  bool activity = false;
  for (int i = 0; i < 200 && !activity; ++i) {
    activity = renderer_->poll_events();
    if (!activity)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(activity);

  renderer_->begin_frame();
  EXPECT_FLOAT_EQ(ImGui::GetIO().MousePos.x, 42.0f);
  EXPECT_FLOAT_EQ(ImGui::GetIO().MousePos.y, 17.0f);
  renderer_->end_frame();

  ::close(sock);
}

TEST_F(WebRendererTest, BeginFrame_DrainsQueuedResizeIntoImGuiIO) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);

  // See BeginFrame_DrainsQueuedMouseMoveIntoImGuiIO: drain the
  // connect-triggered activity signal first.
  for (int i = 0; i < 200; ++i) {
    if (renderer_->poll_events())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::vector<std::byte> payload;
  push_f32(payload, 640.0f);
  push_f32(payload, 480.0f);
  push_f32(payload, 2.0f);
  send_ws_binary(sock, build_envelope(web_msg_type::resize, payload));

  bool activity = false;
  for (int i = 0; i < 200 && !activity; ++i) {
    activity = renderer_->poll_events();
    if (!activity)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(activity);

  renderer_->begin_frame();
  EXPECT_FLOAT_EQ(ImGui::GetIO().DisplaySize.x, 640.0f);
  EXPECT_FLOAT_EQ(ImGui::GetIO().DisplaySize.y, 480.0f);
  EXPECT_FLOAT_EQ(ImGui::GetIO().DisplayFramebufferScale.x, 2.0f);
  renderer_->end_frame();

  ::close(sock);
}

// ── draw_protocol: encode_frame ─────────────────────────────────────────────

TEST_F(WebRendererTest, EncodeFrame_RoundTripsVertexAndIndexCounts) {
  const ImDrawData* draw_data = render_test_frame();
  ASSERT_NE(draw_data, nullptr);
  ASSERT_TRUE(draw_data->Valid);

  auto bytes = encode_frame(*draw_data);

  size_t pos = 0;
  ASSERT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::frame));
  pos += 3; // reserved
  uint32_t payload_len = read_u32(bytes, pos);
  EXPECT_EQ(payload_len, bytes.size() - 8);

  pos += 4 * 6; // display pos, display size, framebuffer scale
  uint32_t total_vtx = read_u32(bytes, pos);
  uint32_t total_idx = read_u32(bytes, pos);
  uint32_t cmd_list_count = read_u32(bytes, pos);
  EXPECT_EQ(total_vtx, static_cast<uint32_t>(draw_data->TotalVtxCount));
  EXPECT_EQ(total_idx, static_cast<uint32_t>(draw_data->TotalIdxCount));
  EXPECT_EQ(cmd_list_count, static_cast<uint32_t>(draw_data->CmdListsCount));
}

TEST_F(WebRendererTest, EncodeFrame_ClipRectAndTextureIdPerDrawCmd) {
  const ImDrawData* draw_data = render_test_frame();
  ASSERT_NE(draw_data, nullptr);

  auto bytes = encode_frame(*draw_data);

  size_t pos = 8;   // envelope header
  pos += 4 * 6;     // display pos/size/scale
  pos += 4 * 2;     // total_vtx_count, total_idx_count
  uint32_t cmd_list_count = read_u32(bytes, pos);
  ASSERT_EQ(cmd_list_count, static_cast<uint32_t>(draw_data->CmdListsCount));

  for (int i = 0; i < draw_data->CmdListsCount; ++i) {
    const ImDrawList& cmd_list = *draw_data->CmdLists[i];

    uint32_t vtx_count = read_u32(bytes, pos);
    uint32_t idx_count = read_u32(bytes, pos);
    EXPECT_EQ(vtx_count, static_cast<uint32_t>(cmd_list.VtxBuffer.Size));
    EXPECT_EQ(idx_count, static_cast<uint32_t>(cmd_list.IdxBuffer.Size));
    pos += static_cast<size_t>(vtx_count) * sizeof(ImDrawVert);
    pos += static_cast<size_t>(idx_count) * sizeof(ImDrawIdx);

    uint32_t real_cmd_count = read_u32(bytes, pos);
    uint32_t expected_real_cmd_count = 0;
    for (const ImDrawCmd& cmd : cmd_list.CmdBuffer)
      if (!cmd.UserCallback)
        ++expected_real_cmd_count;
    ASSERT_EQ(real_cmd_count, expected_real_cmd_count);

    for (const ImDrawCmd& cmd : cmd_list.CmdBuffer) {
      if (cmd.UserCallback)
        continue;
      float x0 = read_f32(bytes, pos);
      float y0 = read_f32(bytes, pos);
      float x1 = read_f32(bytes, pos);
      float y1 = read_f32(bytes, pos);
      EXPECT_FLOAT_EQ(x0, cmd.ClipRect.x);
      EXPECT_FLOAT_EQ(y0, cmd.ClipRect.y);
      EXPECT_FLOAT_EQ(x1, cmd.ClipRect.z);
      EXPECT_FLOAT_EQ(y1, cmd.ClipRect.w);

      uint32_t tex_id = read_u32(bytes, pos);
      EXPECT_EQ(tex_id, static_cast<uint32_t>(cmd.GetTexID()));

      uint32_t vtx_offset = read_u32(bytes, pos);
      uint32_t idx_offset = read_u32(bytes, pos);
      uint32_t elem_count = read_u32(bytes, pos);
      EXPECT_EQ(vtx_offset, cmd.VtxOffset);
      EXPECT_EQ(idx_offset, cmd.IdxOffset);
      EXPECT_EQ(elem_count, cmd.ElemCount);
    }
  }
  EXPECT_EQ(pos, bytes.size());
}

// ── draw_protocol: texture encoding ─────────────────────────────────────────

TEST(DrawProtocolTest, EncodeTextureUpdate_WholeTextureOnCreate) {
  ImTextureData tex;
  tex.Create(ImTextureFormat_RGBA32, 4, 3);
  for (int i = 0; i < tex.Width * tex.Height * tex.BytesPerPixel; ++i)
    tex.Pixels[i] = static_cast<unsigned char>(i);

  auto bytes = encode_texture_update(42, tex);

  size_t pos = 0;
  EXPECT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::tex_create));
  pos += 3;
  uint32_t payload_len = read_u32(bytes, pos);
  EXPECT_EQ(payload_len, bytes.size() - 8);

  EXPECT_EQ(read_u32(bytes, pos), 42u);
  EXPECT_EQ(read_u8(bytes, pos), 0); // RGBA32
  EXPECT_EQ(read_u32(bytes, pos), 4u);
  EXPECT_EQ(read_u32(bytes, pos), 3u);
  ASSERT_EQ(read_u32(bytes, pos), 1u); // one rect covering the whole texture
  EXPECT_EQ(read_u32(bytes, pos), 0u); // x
  EXPECT_EQ(read_u32(bytes, pos), 0u); // y
  EXPECT_EQ(read_u32(bytes, pos), 4u); // w
  EXPECT_EQ(read_u32(bytes, pos), 3u); // h

  size_t pixel_bytes = static_cast<size_t>(tex.Width) * tex.Height * tex.BytesPerPixel;
  ASSERT_EQ(bytes.size() - pos, pixel_bytes);
  EXPECT_EQ(std::memcmp(bytes.data() + pos, tex.Pixels, pixel_bytes), 0);
}

TEST(DrawProtocolTest, EncodeTextureUpdate_PartialRectsOnWantUpdates) {
  ImTextureData tex;
  tex.Create(ImTextureFormat_Alpha8, 8, 8);
  for (int i = 0; i < tex.Width * tex.Height; ++i)
    tex.Pixels[i] = static_cast<unsigned char>(i);
  tex.SetTexID(static_cast<ImTextureID>(7));
  tex.SetStatus(ImTextureStatus_OK); // simulate the create having already happened

  ImTextureRect rect{2, 3, 4, 2};
  tex.Updates.push_back(rect);
  tex.SetStatus(ImTextureStatus_WantUpdates);

  auto bytes = encode_texture_update(7, tex);

  size_t pos = 0;
  EXPECT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::tex_update));
  pos += 3;
  uint32_t payload_len = read_u32(bytes, pos);
  EXPECT_EQ(payload_len, bytes.size() - 8);

  EXPECT_EQ(read_u32(bytes, pos), 7u);
  EXPECT_EQ(read_u8(bytes, pos), 1); // Alpha8
  EXPECT_EQ(read_u32(bytes, pos), 8u);
  EXPECT_EQ(read_u32(bytes, pos), 8u);
  ASSERT_EQ(read_u32(bytes, pos), 1u); // only the one updated rect
  EXPECT_EQ(read_u32(bytes, pos), 2u); // x
  EXPECT_EQ(read_u32(bytes, pos), 3u); // y
  EXPECT_EQ(read_u32(bytes, pos), 4u); // w
  EXPECT_EQ(read_u32(bytes, pos), 2u); // h

  // Only the sub-rect's pixels (4*2 = 8 bytes for Alpha8), not the whole
  // 8x8 = 64-byte texture -- the whole point of WantUpdates.
  EXPECT_EQ(bytes.size() - pos, 8u);
}

TEST(DrawProtocolTest, EncodeTextureDestroy_EncodesId) {
  auto bytes = encode_texture_destroy(99);

  size_t pos = 0;
  EXPECT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::tex_destroy));
  pos += 3;
  EXPECT_EQ(read_u32(bytes, pos), 4u);
  EXPECT_EQ(read_u32(bytes, pos), 99u);
}

// ── draw_protocol: input/resize decoding ────────────────────────────────────

TEST(DrawProtocolTest, DecodeInputMessage_MouseMoveRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_move));
  push_f32(payload, 12.5f);
  push_f32(payload, -3.25f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::mouse_move);
  EXPECT_FLOAT_EQ(ev->x, 12.5f);
  EXPECT_FLOAT_EQ(ev->y, -3.25f);
}

TEST(DrawProtocolTest, DecodeInputMessage_MouseButtonRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_button));
  push_u8(payload, 1); // button index
  push_u8(payload, 1); // down
  push_f32(payload, 5.0f);
  push_f32(payload, 6.0f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::mouse_button);
  EXPECT_EQ(ev->button, 1);
  EXPECT_TRUE(ev->down);
  EXPECT_FLOAT_EQ(ev->x, 5.0f);
  EXPECT_FLOAT_EQ(ev->y, 6.0f);
}

TEST(DrawProtocolTest, DecodeInputMessage_MouseWheelRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_wheel));
  push_f32(payload, 0.0f);
  push_f32(payload, -1.5f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::mouse_wheel);
  EXPECT_FLOAT_EQ(ev->wheel_x, 0.0f);
  EXPECT_FLOAT_EQ(ev->wheel_y, -1.5f);
}

TEST(DrawProtocolTest, DecodeInputMessage_KeyRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::key));
  push_u32(payload, 42);
  push_u8(payload, 0); // up

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::key);
  EXPECT_EQ(ev->key_code, 42u);
  EXPECT_FALSE(ev->down);
}

TEST(DrawProtocolTest, DecodeInputMessage_CharRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::char_input));
  push_u32(payload, 0x1F600);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::char_input);
  EXPECT_EQ(ev->codepoint, 0x1F600u);
}

TEST(DrawProtocolTest, DecodeInputMessage_UnknownEventKindReturnsNullopt) {
  std::vector<std::byte> payload;
  push_u8(payload, 0xFF); // not a valid web_input_kind

  EXPECT_FALSE(decode_input_message(build_envelope(web_msg_type::input, payload)).has_value());
}

TEST(DrawProtocolTest, DecodeInputMessage_WrongMsgTypeReturnsNullopt) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_move));
  push_f32(payload, 1.0f);
  push_f32(payload, 2.0f);

  EXPECT_FALSE(decode_input_message(build_envelope(web_msg_type::resize, payload)).has_value());
}

TEST(DrawProtocolTest, DecodeInputMessage_TruncatedPayloadReturnsNullopt) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::mouse_move));
  push_f32(payload, 1.0f);
  // Missing the y coordinate.

  EXPECT_FALSE(decode_input_message(build_envelope(web_msg_type::input, payload)).has_value());
}

TEST(DrawProtocolTest, DecodeResizeMessage_RoundTrips) {
  std::vector<std::byte> payload;
  push_f32(payload, 1024.0f);
  push_f32(payload, 768.0f);
  push_f32(payload, 2.0f);

  auto ev = decode_resize_message(build_envelope(web_msg_type::resize, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_FLOAT_EQ(ev->width, 1024.0f);
  EXPECT_FLOAT_EQ(ev->height, 768.0f);
  EXPECT_FLOAT_EQ(ev->dpr, 2.0f);
}

// ── wire-format assumptions ──────────────────────────────────────────────────
//
// Regression guard: the FRAME payload hard-codes a 16-bit index width (see
// draw_protocol.cpp's static_assert). If this repo's ImGui config ever
// overrides ImDrawIdx, this test fails loudly in CI instead of the wire
// format silently corrupting.

TEST(DrawProtocolTest, ImDrawIdxIsTwoBytes) {
  EXPECT_EQ(sizeof(ImDrawIdx), 2u);
}

// ── civetweb_server: static file serving ────────────────────────────────────
//
// One thin network smoke test, deliberately scoped to plain HTTP GET (built
// by hand over a raw POSIX socket -- wish targets Linux/MSYS2 only, so this
// needs no platform branching, and avoids pulling in a new test-only HTTP
// client dependency). WebSocket handshake/framing is exercised only by the
// draw_protocol unit tests above and by manual browser testing, not here.

TEST(CivetwebServerTest, StaticFileServedOverHttp) {
  auto tmp_dir = std::filesystem::temp_directory_path() /
      ("wish_civetweb_test_" + std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(tmp_dir);
  {
    std::ofstream f(tmp_dir / "hello.txt");
    f << "hello from wish";
  }

  bdg::wish::civetweb_server server("127.0.0.1", 0, tmp_dir);
  server.start();
  int port = server.actual_port();
  ASSERT_GT(port, 0);

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
  ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  std::string request = "GET /hello.txt HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_EQ(::send(sock, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));

  std::string response;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
    response.append(buf, static_cast<size_t>(n));
  ::close(sock);

  server.stop();
  std::filesystem::remove_all(tmp_dir);

  EXPECT_NE(response.find("200"), std::string::npos);
  EXPECT_NE(response.find("hello from wish"), std::string::npos);
}

// ── civetweb_server: WebSocket broadcast ────────────────────────────────────
//
// Validates the actual wire behavior end-to-end at the civetweb_server
// layer: a real WS handshake (hand-built, since accepting doesn't require
// verifying the server's Sec-WebSocket-Accept on the client side) followed
// by parsing one real, unmasked server->client binary frame -- this is the
// mechanism web_renderer::end_frame() depends on to broadcast FRAME/TEX_*
// messages.

TEST(CivetwebServerTest, BroadcastDeliversBinaryFrameOverWebSocket) {
  auto tmp_dir = std::filesystem::temp_directory_path() /
      ("wish_civetweb_ws_test_" + std::to_string(static_cast<long>(::getpid())));
  std::filesystem::create_directories(tmp_dir);

  std::atomic<bool> connected{false};
  bdg::wish::civetweb_server server(
      "127.0.0.1", 0, tmp_dir, [&connected](bdg::wish::ws_connection_id) { connected = true; });
  server.start();
  int port = server.actual_port();
  ASSERT_GT(port, 0);

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
  ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  std::string handshake =
      "GET /ws HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  ASSERT_EQ(::send(sock, handshake.data(), handshake.size(), 0), static_cast<ssize_t>(handshake.size()));

  // Read the HTTP upgrade response headers up to the blank line.
  std::string resp;
  char c;
  while (resp.size() < 8192 && ::recv(sock, &c, 1, 0) == 1) {
    resp.push_back(c);
    if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0)
      break;
  }
  ASSERT_NE(resp.find("101"), std::string::npos) << resp;

  // ws_ready_handler (which flips `connected`) fires once civetweb finishes
  // the handshake on its side; give it a moment relative to our own read.
  for (int i = 0; i < 100 && !connected.load(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_TRUE(connected.load());

  std::vector<std::byte> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
  server.broadcast(payload);

  // One WS frame: 2-byte header (FIN+opcode, MASK+len) then payload.
  // Server->client frames are never masked; the 5-byte test payload needs
  // no extended-length parsing.
  unsigned char header[2];
  ASSERT_EQ(::recv(sock, header, 2, MSG_WAITALL), 2);
  EXPECT_EQ(header[0] & 0x0F, 0x2); // binary opcode
  uint64_t len = header[1] & 0x7F;
  ASSERT_LT(len, 126u);
  std::vector<unsigned char> body(len);
  ASSERT_EQ(::recv(sock, body.data(), len, MSG_WAITALL), static_cast<ssize_t>(len));

  ASSERT_EQ(body.size(), payload.size());
  for (size_t i = 0; i < payload.size(); ++i)
    EXPECT_EQ(body[i], std::to_integer<unsigned char>(payload[i]));

  ::close(sock);
  server.stop();
  std::filesystem::remove_all(tmp_dir);
}
