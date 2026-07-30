// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <web/civetweb_server.hpp>
#include <web/draw_protocol.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>
#include <miniz.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using bdg::wish::web_cache_response;
using bdg::wish::web_input_event;
using bdg::wish::web_input_kind;
using bdg::wish::web_msg_type;
using bdg::wish::web_renderer;
using bdg::wish::web_resize_event;
using bdg::wish::draw_protocol::decode_cache_response_message;
using bdg::wish::draw_protocol::decode_clipboard_text_message;
using bdg::wish::draw_protocol::decode_input_message;
using bdg::wish::draw_protocol::decode_resize_message;
using bdg::wish::draw_protocol::encode_clipboard_write;
using bdg::wish::draw_protocol::encode_frame;
using bdg::wish::draw_protocol::encode_texture_check;
using bdg::wish::draw_protocol::encode_texture_destroy;
using bdg::wish::draw_protocol::encode_texture_update;

// ── byte-buffer helpers (test-local, deliberately independent of
//    draw_protocol.cpp's private reader/writer so tests catch drift against
//    the *documented* wire format, not just against the encoder's own
//    internal logic) ──────────────────────────────────────────────────────

namespace {

// Raw client-socket portability shim: on native Windows sockets are Winsock
// SOCKET handles (not plain fds), closed via closesocket() and requiring
// WSAStartup() before first use -- see bison's socket_transport.cpp for the
// same pattern. MSYS2 and Linux both provide real POSIX sockets.
#if defined(_WIN32)
using raw_socket_t = SOCKET;
constexpr raw_socket_t kInvalidSocket = INVALID_SOCKET;

struct winsock_guard {
  winsock_guard() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~winsock_guard() {
    WSACleanup();
  }
};
const winsock_guard g_winsock_guard;

void close_socket(raw_socket_t sock) {
  ::closesocket(sock);
}
#else
using raw_socket_t = int;
constexpr raw_socket_t kInvalidSocket = -1;

void close_socket(raw_socket_t sock) {
  ::close(sock);
}
#endif

long current_pid() {
#if defined(_WIN32)
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

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
  raw_socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock == kInvalidSocket)
    return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close_socket(sock);
    return -1;
  }

  std::string handshake =
      "GET /ws HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  ::send(sock, handshake.data(), static_cast<int>(handshake.size()), 0);

  std::string resp;
  char c;
  while (resp.size() < 8192 && ::recv(sock, &c, 1, 0) == 1) {
    resp.push_back(c);
    if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0)
      break;
  }
  if (resp.find("101") == std::string::npos) {
    close_socket(sock);
    return -1;
  }
  return static_cast<int>(sock);
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
  ::send(sock, reinterpret_cast<const char*>(frame.data()), static_cast<int>(frame.size()), 0);
}

// One decoded server->client WS binary frame: the envelope's msg_type byte
// plus the already-unwrapped payload bytes.
struct recv_result {
  uint8_t msg_type;
  std::vector<std::byte> payload;
};

// Reads exactly one server->client binary WS frame (never masked) and
// unwraps draw_protocol's envelope. Handles all three RFC6455 length forms
// (7-bit, 16-bit extended, 64-bit extended) -- pending_sync_ resends *every*
// live texture to a newly-connected browser, including the font atlas,
// whose payload is routinely large enough to need extended-length framing,
// so tests must be able to read past it to find a specific small message.
std::optional<recv_result> recv_ws_frame(int sock) {
  unsigned char header[2];
  if (::recv(sock, reinterpret_cast<char*>(header), 2, MSG_WAITALL) != 2)
    return std::nullopt;

  uint64_t len = header[1] & 0x7F;
  if (len == 126) {
    unsigned char ext[2];
    if (::recv(sock, reinterpret_cast<char*>(ext), 2, MSG_WAITALL) != 2)
      return std::nullopt;
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    unsigned char ext[8];
    if (::recv(sock, reinterpret_cast<char*>(ext), 8, MSG_WAITALL) != 8)
      return std::nullopt;
    len = 0;
    for (unsigned char b : ext)
      len = (len << 8) | b;
  }

  std::vector<unsigned char> body(len);
  if (len > 0 &&
      ::recv(sock, reinterpret_cast<char*>(body.data()), static_cast<int>(len), MSG_WAITALL) != static_cast<long>(len))
    return std::nullopt;
  if (body.size() < 8)
    return std::nullopt;

  recv_result result;
  result.msg_type = body[0];
  uint32_t payload_len;
  std::memcpy(&payload_len, body.data() + 4, sizeof(payload_len));
  if (body.size() - 8 != payload_len)
    return std::nullopt;
  result.payload.resize(payload_len);
  for (size_t i = 0; i < payload_len; ++i)
    result.payload[i] = std::byte{body[8 + i]};
  return result;
}

// TEX_CHECK and TEX_CREATE both start with `u32 texture_id, u8 format, u32
// width, ...` -- reads the width field common to either, used to pick our
// 2x2 test texture's message out of a pending_sync_ batch that also resends
// the (much larger) font atlas.
uint32_t recv_payload_width(const std::vector<std::byte>& payload) {
  size_t pos = 5; // texture_id (4) + format (1)
  return read_u32(payload, pos);
}

// Reads WS frames (up to @p max_frames) until one is a TEX_CHECK or
// TEX_CREATE whose width is 2 (our test texture, see write_test_bmp) --
// skipping over any interleaved font-atlas texture messages or FRAME
// broadcasts along the way.
std::optional<recv_result> recv_message_for_test_texture(int sock, int max_frames = 8) {
  for (int i = 0; i < max_frames; ++i) {
    auto msg = recv_ws_frame(sock);
    if (!msg)
      return std::nullopt;
    bool is_tex_msg =
        msg->msg_type == static_cast<uint8_t>(web_msg_type::tex_check) ||
        msg->msg_type == static_cast<uint8_t>(web_msg_type::tex_create);
    if (is_tex_msg && recv_payload_width(msg->payload) == 2)
      return msg;
  }
  return std::nullopt;
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
  EXPECT_TRUE(std::filesystem::exists(dir / "web" / "resource_cache.js"));
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

  close_socket(sock);
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

  close_socket(sock);
}

TEST_F(WebRendererTest, BeginFrame_LeftCtrlKeyEventAlsoSetsMergedModFlag) {
  // Regression test for a bug where ImGui-wide Ctrl-modified shortcuts
  // (Ctrl+A/C/V/X/... in any widget, including TextEditor) silently never
  // fired: IsKeyDown(ImGuiMod_Ctrl) reads a *separate* pseudo-key slot
  // (ImGuiKey_ReservedForModCtrl) that only every official ImGui backend's
  // explicit io.AddKeyEvent(ImGuiMod_Ctrl, ...) call populates -- sending
  // only the literal ImGuiKey_LeftCtrl/RightCtrl events (as this renderer
  // used to) never touches it. See the mod_*_down_ members' doc comment in
  // web_renderer.hpp.
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
  push_u8(payload, static_cast<uint8_t>(web_input_kind::key));
  push_u32(payload, static_cast<uint32_t>(ImGuiKey_LeftCtrl));
  push_u8(payload, 1); // down
  send_ws_binary(sock, build_envelope(web_msg_type::input, payload));

  bool activity = false;
  for (int i = 0; i < 200 && !activity; ++i) {
    activity = renderer_->poll_events();
    if (!activity)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(activity);

  renderer_->begin_frame();
  EXPECT_TRUE(ImGui::IsKeyDown(ImGuiMod_Ctrl));
  EXPECT_TRUE(ImGui::GetIO().KeyCtrl);
  renderer_->end_frame();

  close_socket(sock);
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

  close_socket(sock);
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

  uint32_t target_id = read_u32(bytes, pos);
  EXPECT_EQ(target_id, 0u); // default target_id -- the canvas

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
  pos += 4;         // target_id
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

TEST_F(WebRendererTest, EncodeFrame_NonZeroTargetIdRoundTrips) {
  const ImDrawData* draw_data = render_test_frame();
  ASSERT_NE(draw_data, nullptr);

  auto bytes = encode_frame(*draw_data, /*target_id=*/42);

  size_t pos = 8; // envelope header
  EXPECT_EQ(read_u32(bytes, pos), 42u);
}

// ── get_or_load_texture() ─────────────────────────────────────────────────────

namespace {

// Writes a minimal, uncompressed 2x2 24-bit BMP (a format stb_image decodes
// without any external codec) to `path`. Row size (2 * 3 = 6 bytes) is
// padded to a multiple of 4, per the BMP spec.
void write_test_bmp(const std::filesystem::path& path) {
  std::vector<uint8_t> bytes;
  auto put_u16 = [&](uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xFF));
    bytes.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto put_u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i)
      bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
  };

  const uint32_t pixel_data_offset = 14 + 40;
  const uint32_t row_size = 8; // 2 px * 3 bytes, padded to a multiple of 4
  const uint32_t pixel_data_size = row_size * 2;
  const uint32_t file_size = pixel_data_offset + pixel_data_size;

  // BITMAPFILEHEADER
  bytes.push_back('B');
  bytes.push_back('M');
  put_u32(file_size);
  put_u32(0); // reserved
  put_u32(pixel_data_offset);

  // BITMAPINFOHEADER
  put_u32(40); // header size
  put_u32(2);  // width
  put_u32(2);  // height
  put_u16(1);  // planes
  put_u16(24); // bits per pixel
  put_u32(0);  // compression = BI_RGB
  put_u32(pixel_data_size);
  put_u32(0); // x pixels per meter
  put_u32(0); // y pixels per meter
  put_u32(0); // colors used
  put_u32(0); // colors important

  // Pixel data: bottom-up rows, BGR order, 2px padding to row_size.
  for (int row = 0; row < 2; ++row) {
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(255); // red pixel
    bytes.push_back(0);
    bytes.push_back(255);
    bytes.push_back(0); // green pixel
    bytes.push_back(0); // row padding
    bytes.push_back(0);
  }

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_F(WebRendererTest, GetOrLoadTexture_MissingFileReturnsNullAndIsCached) {
  ImTextureID first = renderer_->get_or_load_texture("does_not_exist.bmp", std::filesystem::temp_directory_path());
  EXPECT_EQ(first, ImTextureID{});

  ImTextureID second = renderer_->get_or_load_texture("does_not_exist.bmp", std::filesystem::temp_directory_path());
  EXPECT_EQ(second, ImTextureID{});
}

TEST_F(WebRendererTest, GetOrLoadTexture_DecodesAndUploadsThroughTextureWalk) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_get_or_load_texture_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  // begin_frame()/end_frame() bracket the load, mirroring how render_node()
  // calls get_or_load_texture() mid-frame in imgui_ui_renderer.cpp. The
  // texture is registered with ImGui as soon as it decodes, but its real id
  // is only assigned once end_frame() calls ImGui::Render() (which walks
  // ImDrawData::Textures and resolves WantCreate) -- so, like
  // get_or_load_font()'s first-call contract, this first call returns null.
  renderer_->begin_frame();
  ImGui::Begin("test");
  ImTextureID first = renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  EXPECT_EQ(first, ImTextureID{});

  ImTextureData* uploaded = nullptr;
  for (ImTextureData* t : *ImGui::GetDrawData()->Textures) {
    if (t->Width == 2 && t->Height == 2 && t->Format == ImTextureFormat_RGBA32)
      uploaded = t;
  }
  ASSERT_NE(uploaded, nullptr);
  EXPECT_EQ(uploaded->Status, ImTextureStatus_OK); // end_frame() already resolved WantCreate
  EXPECT_NE(uploaded->TexID, ImTextureID{});

  // Now that end_frame() has assigned a real id, a follow-up call for the
  // same path returns it from cache -- no re-decode, no second registration.
  ImTextureID cached = renderer_->get_or_load_texture("swatch.bmp", dir);
  EXPECT_EQ(cached, uploaded->TexID);

  std::filesystem::remove_all(dir);
}

// ── get_or_load_texture(): CRC32 + cacheable metadata ───────────────────────

TEST_F(WebRendererTest, GetOrLoadTexture_ComputesCrc32OnTheFlyWhenNoPrecomputedMap) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_texmeta_onthefly_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  std::vector<unsigned char> file_bytes;
  {
    std::ifstream file(dir / "swatch.bmp", std::ios::binary);
    file_bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  uint32_t expected_crc32 = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, file_bytes.data(), file_bytes.size()));

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir); // embedded_crc32s defaults to nullptr
  ImGui::End();
  renderer_->end_frame();

  auto meta = renderer_->texture_meta_for_test("swatch.bmp");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->src, "swatch.bmp");
  EXPECT_EQ(meta->crc32, expected_crc32);
  EXPECT_TRUE(meta->cacheable);

  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, GetOrLoadTexture_ReusesPrecomputedCrc32InsteadOfRecomputing) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_texmeta_precomputed_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  // Deliberately wrong/sentinel value: if get_or_load_texture recomputed the
  // CRC32 from the file's real bytes instead of reusing this map entry, the
  // real (non-sentinel) value would show up in texture_meta_ instead.
  constexpr uint32_t kSentinelCrc32 = 0xDEADBEEFu;
  std::unordered_map<std::string, uint32_t> embedded_crc32s{{"swatch.bmp", kSentinelCrc32}};

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir, &embedded_crc32s);
  ImGui::End();
  renderer_->end_frame();

  auto meta = renderer_->texture_meta_for_test("swatch.bmp");
  ASSERT_TRUE(meta.has_value());
  EXPECT_EQ(meta->crc32, kSentinelCrc32);

  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, GetOrLoadTexture_PrivatePathIsNotCacheable) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_texmeta_private_test";
  std::filesystem::create_directories(dir / "private");
  write_test_bmp(dir / "private" / "photo.bmp");
  write_test_bmp(dir / "public.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture((dir / "private" / "photo.bmp").string(), dir);
  renderer_->get_or_load_texture("public.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  auto private_meta = renderer_->texture_meta_for_test((dir / "private" / "photo.bmp").string());
  ASSERT_TRUE(private_meta.has_value());
  EXPECT_FALSE(private_meta->cacheable);

  auto public_meta = renderer_->texture_meta_for_test("public.bmp");
  ASSERT_TRUE(public_meta.has_value());
  EXPECT_TRUE(public_meta->cacheable);

  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, GetOrLoadTexture_RepeatedCallForCachedSrcKeepsMetadataAndDoesNotCrash) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_texmeta_repeat_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  auto first = renderer_->texture_meta_for_test("swatch.bmp");
  ASSERT_TRUE(first.has_value());

  EXPECT_NO_THROW(renderer_->get_or_load_texture("swatch.bmp", dir));
  auto second = renderer_->texture_meta_for_test("swatch.bmp");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->crc32, first->crc32);
  EXPECT_EQ(second->cacheable, first->cacheable);

  std::filesystem::remove_all(dir);
}

// ── pending_sync_ cache-check handshake ─────────────────────────────────────
//
// End-to-end through real WebSocket connections: a (re)connecting browser
// must receive TEX_CHECK (not a full TEX_CREATE) for a cacheable texture,
// and only get pixels after replying "miss". Non-cacheable (private/)
// textures must be completely unaffected -- same plain TEX_CREATE as today.

namespace {

// Waits (via poll_events()) for the on_message/on_connect callback on a
// civetweb worker thread to signal activity, mirroring the pattern used by
// the existing input/resize tests above.
void wait_for_activity(web_renderer& renderer) {
  for (int i = 0; i < 200; ++i) {
    if (renderer.poll_events())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

} // namespace

TEST_F(WebRendererTest, PendingSync_CacheableTextureSendsCheckNotCreate) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_pending_sync_cacheable_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame(); // texture reaches ImTextureStatus_OK this frame

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_); // drain the connect-triggered activity signal

  renderer_->begin_frame();
  renderer_->end_frame(); // drains pending_sync_ for the new connection

  // pending_sync_ also resends the font atlas (not cacheable) in the same
  // batch; recv_message_for_test_texture() skips past it to find ours.
  auto msg = recv_message_for_test_texture(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::tex_check));

  close_socket(sock);
  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, WantCreate_CacheableTextureSendsCheckToAlreadyConnectedClient) {
  // The common case: a browser tab is already connected (e.g. viewing the
  // app) when a cacheable texture is loaded for the very first time this
  // session -- that first WantCreate transition should offer a TEX_CHECK to
  // the already-connected client instead of unconditionally paying the full
  // pixel upload, since that client may have this exact (path, crc32)
  // persisted from an earlier run of the app.
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_want_create_cacheable_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_); // drain the connect-triggered activity signal

  // No texture loaded yet on the first frame -- just resolves the font
  // atlas's own WantCreate and drains pending_sync_ for the connection
  // above (as a plain TEX_CREATE, since the font atlas is never cacheable).
  renderer_->begin_frame();
  ImGui::Begin("test");
  ImGui::End();
  renderer_->end_frame();

  // Now load the texture, on a frame *after* the client already connected.
  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame(); // texture's first-ever WantCreate happens here

  auto msg = recv_message_for_test_texture(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::tex_check));

  close_socket(sock);
  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, PendingSync_PrivatePathStillSendsFullCreate) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_pending_sync_private_test";
  std::filesystem::create_directories(dir / "private");
  write_test_bmp(dir / "private" / "photo.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("private/photo.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  renderer_->begin_frame();
  renderer_->end_frame();

  auto msg = recv_message_for_test_texture(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::tex_create));

  close_socket(sock);
  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, CacheResponse_MissTriggersFullTexCreate) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_cache_response_miss_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  renderer_->begin_frame();
  renderer_->end_frame();

  auto check = recv_message_for_test_texture(sock);
  ASSERT_TRUE(check.has_value());
  ASSERT_EQ(check->msg_type, static_cast<uint8_t>(web_msg_type::tex_check));
  size_t pos = 0;
  uint32_t texture_id = read_u32(check->payload, pos);

  std::vector<std::byte> resp_payload;
  push_u32(resp_payload, texture_id);
  push_u8(resp_payload, 0); // miss
  send_ws_binary(sock, build_envelope(web_msg_type::cache_response, resp_payload));
  wait_for_activity(*renderer_);

  renderer_->begin_frame();
  renderer_->end_frame(); // drains cache_response_queue_

  // Skips the FRAME broadcast left over from the previous end_frame() call
  // to find the resulting TEX_CREATE.
  auto create = recv_message_for_test_texture(sock);
  ASSERT_TRUE(create.has_value());
  EXPECT_EQ(create->msg_type, static_cast<uint8_t>(web_msg_type::tex_create));
  pos = 0;
  EXPECT_EQ(read_u32(create->payload, pos), texture_id);

  close_socket(sock);
  std::filesystem::remove_all(dir);
}

TEST_F(WebRendererTest, CacheResponse_HitSendsNothingFurtherForThatTexture) {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_cache_response_hit_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  renderer_->begin_frame();
  ImGui::Begin("test");
  renderer_->get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer_->end_frame();

  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  renderer_->begin_frame();
  renderer_->end_frame();

  auto check = recv_message_for_test_texture(sock);
  ASSERT_TRUE(check.has_value());
  ASSERT_EQ(check->msg_type, static_cast<uint8_t>(web_msg_type::tex_check));
  size_t pos = 0;
  uint32_t texture_id = read_u32(check->payload, pos);

  std::vector<std::byte> resp_payload;
  push_u32(resp_payload, texture_id);
  push_u8(resp_payload, 1); // hit
  send_ws_binary(sock, build_envelope(web_msg_type::cache_response, resp_payload));
  wait_for_activity(*renderer_);

  // No new widgets this frame (e.g. no first-use of a text glyph, which
  // would trigger an incremental font-atlas TEX_UPDATE broadcast to every
  // connection and confuse the assertion below) -- just drain the queues.
  renderer_->begin_frame();
  renderer_->end_frame(); // drains cache_response_queue_ (hit -> nothing sent) then broadcasts FRAME

  // Two FRAME broadcasts are owed to this connection at this point (one
  // from the pending_sync_ end_frame() above, one from this one); neither
  // of them should be interleaved with another TEX_CREATE/TEX_CHECK for our
  // texture -- that would mean the hit failed to suppress the pixel resend.
  for (int i = 0; i < 2; ++i) {
    auto next = recv_ws_frame(sock);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->msg_type, static_cast<uint8_t>(web_msg_type::frame));
  }

  close_socket(sock);
  std::filesystem::remove_all(dir);
}

// ── offscreen render target ─────────────────────────────────────────────────
//
// Mirrors test_sdl3_renderer.cpp's begin_render_target()/end_render_target()/
// flush_draw_list() coverage, adapted to inspect broadcast wire messages
// instead of GPU pixel readback -- there's no real GPU here (see "Offscreen
// Render Targets" in src/web/DESIGN.md).

TEST_F(WebRendererTest, BeginRenderTarget_ReturnsNonNullIdAndReusesForSameSize) {
  ImTextureID tex = renderer_->begin_render_target(64, 64);
  EXPECT_NE(tex, ImTextureID{});
  renderer_->end_render_target();

  ImTextureID tex2 = renderer_->begin_render_target(64, 64);
  EXPECT_EQ(tex2, tex); // same size -> cached id reused
  renderer_->end_render_target();
}

TEST_F(WebRendererTest, BeginRenderTarget_NonPositiveSizeReturnsNull) {
  EXPECT_EQ(renderer_->begin_render_target(0, 4), ImTextureID{});
  EXPECT_EQ(renderer_->begin_render_target(4, -1), ImTextureID{});
}

TEST_F(WebRendererTest, BeginRenderTarget_SizeChangeAllocatesNewIdAndBroadcastsDestroyForOld) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  ImTextureID tex1 = renderer_->begin_render_target(4, 4);
  ASSERT_NE(tex1, ImTextureID{});
  renderer_->end_render_target();

  ImTextureID tex2 = renderer_->begin_render_target(8, 8);
  ASSERT_NE(tex2, ImTextureID{});
  EXPECT_NE(tex2, tex1); // size changed -> new id, old one torn down

  auto msg = recv_ws_frame(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::tex_destroy));
  size_t pos = 0;
  EXPECT_EQ(read_u32(msg->payload, pos), static_cast<uint32_t>(tex1));

  renderer_->end_render_target();
  close_socket(sock);
}

TEST_F(WebRendererTest, EndRenderTargetWithoutBeginIsSafeNoOp) {
  EXPECT_NO_THROW(renderer_->end_render_target());
}

TEST_F(WebRendererTest, FlushDrawList_WhileTargetActiveSendsFrameTaggedWithTargetId) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  ImTextureID target_tex = renderer_->begin_render_target(4, 4);
  ASSERT_NE(target_tex, ImTextureID{});

  // A small quad referencing an arbitrary (never-uploaded) texture id --
  // flush_draw_list() never touches texture upload state, only whatever
  // draw_list already references, mirroring the sdl3 test's own approach.
  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  draw_list.PushClipRect(ImVec2(0, 0), ImVec2(4, 4));
  draw_list.AddImageQuad(
      static_cast<ImTextureID>(7), ImVec2(0, 0), ImVec2(4, 0), ImVec2(4, 4), ImVec2(0, 4));
  draw_list.PopClipRect();

  renderer_->flush_draw_list(draw_list, 4, 4);
  renderer_->end_render_target();

  auto msg = recv_ws_frame(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::frame));
  size_t pos = 0;
  EXPECT_EQ(read_u32(msg->payload, pos), static_cast<uint32_t>(target_tex));
  pos += 4 * 2; // DisplayPos
  EXPECT_FLOAT_EQ(read_f32(msg->payload, pos), 4.0f); // DisplaySize.x
  EXPECT_FLOAT_EQ(read_f32(msg->payload, pos), 4.0f); // DisplaySize.y

  close_socket(sock);
}

TEST_F(WebRendererTest, FlushDrawList_OutsideRenderTargetTagsFrameWithZero) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  draw_list.PushClipRect(ImVec2(0, 0), ImVec2(4, 4));
  draw_list.AddImageQuad(
      static_cast<ImTextureID>(7), ImVec2(0, 0), ImVec2(4, 0), ImVec2(4, 4), ImVec2(0, 4));
  draw_list.PopClipRect();

  renderer_->flush_draw_list(draw_list, 4, 4); // no begin_render_target() -- targets the canvas

  auto msg = recv_ws_frame(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::frame));
  size_t pos = 0;
  EXPECT_EQ(read_u32(msg->payload, pos), 0u);

  close_socket(sock);
}

TEST_F(WebRendererTest, FlushDrawListWithNonPositiveSizeIsSafeNoOpAndSendsNothing) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);
  wait_for_activity(*renderer_);

  ImDrawList draw_list(ImGui::GetDrawListSharedData());
  draw_list._ResetForNewFrame();
  EXPECT_NO_THROW(renderer_->flush_draw_list(draw_list, 0, 0));
  EXPECT_NO_THROW(renderer_->flush_draw_list(draw_list, -1, 4));

  // Prove nothing was broadcast for either non-positive call above: a
  // deliberate follow-up flush must be the very first message this
  // connection receives.
  ImTextureID target_tex = renderer_->begin_render_target(2, 2);
  ASSERT_NE(target_tex, ImTextureID{});
  renderer_->flush_draw_list(draw_list, 2, 2);
  renderer_->end_render_target();

  auto msg = recv_ws_frame(sock);
  ASSERT_TRUE(msg.has_value());
  EXPECT_EQ(msg->msg_type, static_cast<uint8_t>(web_msg_type::frame));

  close_socket(sock);
}

TEST(WebRendererCacheHandshakeTeardownTest, Teardown_ClearsCacheHandshakeStateWithoutCrash) {
  web_renderer renderer("127.0.0.1", 0, 16);
  renderer.setup();

  std::filesystem::path dir = std::filesystem::temp_directory_path() / "wish_cache_handshake_teardown_test";
  std::filesystem::create_directories(dir);
  write_test_bmp(dir / "swatch.bmp");

  renderer.begin_frame();
  ImGui::Begin("test");
  renderer.get_or_load_texture("swatch.bmp", dir);
  ImGui::End();
  renderer.end_frame();

  EXPECT_NO_THROW(renderer.teardown());
  std::filesystem::remove_all(dir);
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

// ── draw_protocol: TEX_CHECK / CACHE_RESPONSE ───────────────────────────────

TEST(DrawProtocolTest, EncodeTextureCheck_PayloadLayout) {
  ImTextureData tex;
  tex.Create(ImTextureFormat_RGBA32, 4, 3);

  std::string path = "res/icons/folder.png";
  auto bytes = encode_texture_check(42, path, 0xCAFEBABEu, tex);

  size_t pos = 0;
  EXPECT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::tex_check));
  pos += 3;
  uint32_t payload_len = read_u32(bytes, pos);
  EXPECT_EQ(payload_len, bytes.size() - 8);

  EXPECT_EQ(read_u32(bytes, pos), 42u);
  EXPECT_EQ(read_u8(bytes, pos), 0); // RGBA32
  EXPECT_EQ(read_u32(bytes, pos), 4u);
  EXPECT_EQ(read_u32(bytes, pos), 3u);
  EXPECT_EQ(read_u32(bytes, pos), 0xCAFEBABEu);

  uint32_t path_len = read_u32(bytes, pos);
  ASSERT_EQ(path_len, path.size());
  std::string decoded_path(reinterpret_cast<const char*>(bytes.data() + pos), path_len);
  EXPECT_EQ(decoded_path, path);
  pos += path_len;

  EXPECT_EQ(pos, bytes.size());
}

TEST(DrawProtocolTest, EncodeTextureCheck_Alpha8FormatByte) {
  ImTextureData tex;
  tex.Create(ImTextureFormat_Alpha8, 8, 8);

  auto bytes = encode_texture_check(7, "res/fonts/default.ttf", 1, tex);

  size_t pos = 0;
  pos += 8; // envelope
  pos += 4; // texture_id
  EXPECT_EQ(read_u8(bytes, pos), 1); // Alpha8
}

TEST(DrawProtocolTest, DecodeCacheResponse_RoundTripsHit) {
  std::vector<std::byte> payload;
  push_u32(payload, 42);
  push_u8(payload, 1); // hit

  auto resp = decode_cache_response_message(build_envelope(web_msg_type::cache_response, payload));
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->texture_id, 42u);
  EXPECT_TRUE(resp->hit);
}

TEST(DrawProtocolTest, DecodeCacheResponse_RoundTripsMiss) {
  std::vector<std::byte> payload;
  push_u32(payload, 7);
  push_u8(payload, 0); // miss

  auto resp = decode_cache_response_message(build_envelope(web_msg_type::cache_response, payload));
  ASSERT_TRUE(resp.has_value());
  EXPECT_EQ(resp->texture_id, 7u);
  EXPECT_FALSE(resp->hit);
}

TEST(DrawProtocolTest, DecodeCacheResponse_RejectsWrongMsgType) {
  std::vector<std::byte> payload;
  push_u32(payload, 42);
  push_u8(payload, 1);

  EXPECT_FALSE(decode_cache_response_message(build_envelope(web_msg_type::tex_check, payload)).has_value());
}

TEST(DrawProtocolTest, DecodeCacheResponse_RejectsTruncatedPayload) {
  std::vector<std::byte> payload;
  push_u32(payload, 42);
  // Missing the hit byte.

  EXPECT_FALSE(decode_cache_response_message(build_envelope(web_msg_type::cache_response, payload)).has_value());
}

// ── draw_protocol: CLIPBOARD_TEXT / CLIPBOARD_WRITE ─────────────────────────

TEST(DrawProtocolTest, DecodeClipboardText_RoundTripsUtf8) {
  const std::string text = "hello \xE2\x9C\x93 clipboard"; // includes a UTF-8 checkmark
  std::vector<std::byte> payload(text.size());
  std::memcpy(payload.data(), text.data(), text.size());

  auto decoded = decode_clipboard_text_message(build_envelope(web_msg_type::clipboard_text, payload));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, text);
}

TEST(DrawProtocolTest, DecodeClipboardText_EmptyPayloadRoundTrips) {
  auto decoded = decode_clipboard_text_message(build_envelope(web_msg_type::clipboard_text, {}));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(decoded->empty());
}

TEST(DrawProtocolTest, DecodeClipboardText_RejectsWrongMsgType) {
  std::vector<std::byte> payload(3);
  EXPECT_FALSE(decode_clipboard_text_message(build_envelope(web_msg_type::resize, payload)).has_value());
}

TEST(DrawProtocolTest, EncodeClipboardWrite_EncodesTextVerbatim) {
  auto bytes = encode_clipboard_write("copied text");

  size_t pos = 0;
  EXPECT_EQ(read_u8(bytes, pos), static_cast<uint8_t>(web_msg_type::clipboard_write));
  pos += 3;
  ASSERT_EQ(read_u32(bytes, pos), 11u);
  std::string payload(reinterpret_cast<const char*>(bytes.data()) + 8, 11);
  EXPECT_EQ(payload, "copied text");
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

TEST(DrawProtocolTest, DecodeInputMessage_TouchDownRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::touch_down));
  push_u32(payload, 7); // touch_id
  push_f32(payload, 100.0f);
  push_f32(payload, 200.0f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::touch_down);
  EXPECT_EQ(ev->touch_id, 7u);
  EXPECT_FLOAT_EQ(ev->x, 100.0f);
  EXPECT_FLOAT_EQ(ev->y, 200.0f);
}

TEST(DrawProtocolTest, DecodeInputMessage_TouchMoveRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::touch_move));
  push_u32(payload, 7);
  push_f32(payload, 110.0f);
  push_f32(payload, 205.0f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::touch_move);
  EXPECT_EQ(ev->touch_id, 7u);
  EXPECT_FLOAT_EQ(ev->x, 110.0f);
  EXPECT_FLOAT_EQ(ev->y, 205.0f);
}

TEST(DrawProtocolTest, DecodeInputMessage_TouchUpAndCancelRoundTrip) {
  for (web_input_kind kind : {web_input_kind::touch_up, web_input_kind::touch_cancel}) {
    std::vector<std::byte> payload;
    push_u8(payload, static_cast<uint8_t>(kind));
    push_u32(payload, 3);
    push_f32(payload, 1.0f);
    push_f32(payload, 2.0f);

    auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->kind, kind);
    EXPECT_EQ(ev->touch_id, 3u);
  }
}

TEST(DrawProtocolTest, DecodeInputMessage_MotionRoundTrips) {
  std::vector<std::byte> payload;
  push_u8(payload, static_cast<uint8_t>(web_input_kind::motion));
  push_f32(payload, 0.5f);
  push_f32(payload, -9.8f);
  push_f32(payload, 0.1f);

  auto ev = decode_input_message(build_envelope(web_msg_type::input, payload));
  ASSERT_TRUE(ev.has_value());
  EXPECT_EQ(ev->kind, web_input_kind::motion);
  EXPECT_FLOAT_EQ(ev->accel_x, 0.5f);
  EXPECT_FLOAT_EQ(ev->accel_y, -9.8f);
  EXPECT_FLOAT_EQ(ev->accel_z, 0.1f);
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
// by hand over a raw client socket -- see the raw_socket_t shim above for
// the Winsock/POSIX split -- avoiding a new test-only HTTP client
// dependency). WebSocket handshake/framing is exercised only by the
// draw_protocol unit tests above and by manual browser testing, not here.

TEST(CivetwebServerTest, StaticFileServedOverHttp) {
  auto tmp_dir = std::filesystem::temp_directory_path() /
      ("wish_civetweb_test_" + std::to_string(current_pid()));
  std::filesystem::create_directories(tmp_dir);
  {
    std::ofstream f(tmp_dir / "hello.txt");
    f << "hello from wish";
  }

  bdg::wish::civetweb_server server("127.0.0.1", 0, tmp_dir);
  server.start();
  int port = server.actual_port();
  ASSERT_GT(port, 0);

  raw_socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(sock, kInvalidSocket);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr), 1);
  ASSERT_EQ(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  std::string request = "GET /hello.txt HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  ASSERT_EQ(::send(sock, request.data(), static_cast<int>(request.size()), 0), static_cast<long>(request.size()));

  std::string response;
  char buf[4096];
  long n;
  while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
    response.append(buf, static_cast<size_t>(n));
  close_socket(sock);

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
      ("wish_civetweb_ws_test_" + std::to_string(current_pid()));
  std::filesystem::create_directories(tmp_dir);

  std::atomic<bool> connected{false};
  bdg::wish::civetweb_server server(
      "127.0.0.1", 0, tmp_dir, [&connected](bdg::wish::ws_connection_id) { connected = true; });
  server.start();
  int port = server.actual_port();
  ASSERT_GT(port, 0);

  raw_socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(sock, kInvalidSocket);
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
  ASSERT_EQ(::send(sock, handshake.data(), static_cast<int>(handshake.size()), 0),
            static_cast<long>(handshake.size()));

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
  ASSERT_EQ(::recv(sock, reinterpret_cast<char*>(header), 2, MSG_WAITALL), 2);
  EXPECT_EQ(header[0] & 0x0F, 0x2); // binary opcode
  uint64_t len = header[1] & 0x7F;
  ASSERT_LT(len, 126u);
  std::vector<unsigned char> body(len);
  ASSERT_EQ(::recv(sock, reinterpret_cast<char*>(body.data()), static_cast<int>(len), MSG_WAITALL), static_cast<long>(len));

  ASSERT_EQ(body.size(), payload.size());
  for (size_t i = 0; i < payload.size(); ++i)
    EXPECT_EQ(body[i], std::to_integer<unsigned char>(payload[i]));

  close_socket(sock);
  server.stop();
  std::filesystem::remove_all(tmp_dir);
}
