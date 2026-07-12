// MIT License © 2025 Binary Dice Games
//
// Mirrors test_web_renderer.cpp's real-socket, no-browser-required style
// (see src/automation/DESIGN.md's "Tests" section): pure-logic tests for
// automation::parse_query_tree_request()/build_tree_snapshot() need no
// networking at all, plus one end-to-end test that drives an actual
// QUERY_TREE -> TREE_SNAPSHOT round trip over a real WebSocket connection.
#include <gtest/gtest.h>

#include <automation/automation_query.hpp>
#include <context/context.hpp>
#include <server/registry.hpp>
#include <ui/ui_importer.hpp>
#include <web/draw_protocol.hpp>
#include <web/web_renderer.hpp>

#include <imgui.h>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using bdg::wish::automation::build_tree_snapshot;
using bdg::wish::automation::hit_test_entry;
using bdg::wish::automation::hit_test_map;
using bdg::wish::automation::parse_query_tree_request;
using bdg::wish::context;
using bdg::wish::web_renderer;
using bdg::wish::draw_protocol::decode_query_tree_message;
using bdg::wish::draw_protocol::encode_tree_snapshot;

using namespace bdg::bison;

namespace {
// import_json() alone never assigns __wish_id (that's done by
// ui_template::do_instantiate() as part of RMI object registration -- see
// calculator.cpp's on_init() for the production pattern). Tests that need
// hit_test_map_ to join against a plain import_json() tree assign their own
// ids directly; a real RMI-driven id from rmi::shared::generate_id() isn't
// needed here, just any nonzero value.
void assign_wish_ids(bdg::wish::ui_tree& tree) {
  uint32_t next_id = 1;
  for (auto& [path, elem] : tree)
    (*elem)["__wish_id"_key] = bdg::bison::key_t{next_id++};
}
} // namespace

// ── parse_query_tree_request() ──────────────────────────────────────────────

TEST(ParseQueryTreeRequestTest, ParsesRequestIdAndRoot) {
  auto req = parse_query_tree_request(R"({"request_id":7,"root":"dialog"})");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->request_id, 7u);
  EXPECT_EQ(req->root, "dialog");
}

TEST(ParseQueryTreeRequestTest, MissingRootDefaultsToEmpty) {
  auto req = parse_query_tree_request(R"({"request_id":3})");
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->request_id, 3u);
  EXPECT_EQ(req->root, "");
}

TEST(ParseQueryTreeRequestTest, MissingRequestIdReturnsNullopt) {
  EXPECT_FALSE(parse_query_tree_request(R"({"root":""})").has_value());
}

TEST(ParseQueryTreeRequestTest, MalformedJsonReturnsNullopt) {
  EXPECT_FALSE(parse_query_tree_request("not json").has_value());
}

TEST(ParseQueryTreeRequestTest, NonObjectJsonReturnsNullopt) {
  EXPECT_FALSE(parse_query_tree_request("[1,2,3]").has_value());
}

// ── build_tree_snapshot() ───────────────────────────────────────────────────

class BuildTreeSnapshotTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    bdg::wish::register_all(); // populates DisplayName attributes build_tree_snapshot()
                                // relies on for the "class" field
  }

  BuildTreeSnapshotTest() {
    assign_wish_ids(tree_);
  }

  bdg::wish::ui_tree tree_ = bdg::wish::import_json(R"({
    "type": "Window", "title": "Dialog",
    "children": {
      "ok":  { "type": "Button", "label": "OK" },
      "msg": { "type": "Label",  "text": "hello" }
    }
  })");
};

TEST_F(BuildTreeSnapshotTest, IncludesPathClassAndProbedField) {
  hit_test_map hits;
  auto json_text = build_tree_snapshot(1, "", tree_, hits);
  auto j = nlohmann::json::parse(json_text);

  EXPECT_EQ(j["request_id"], 1);
  ASSERT_TRUE(j["widgets"].is_array());
  EXPECT_EQ(j["widgets"].size(), 3u); // root Window + Button + Label

  bool found_button = false;
  for (auto& w : j["widgets"]) {
    if (w["path"] == "ok") {
      found_button = true;
      EXPECT_EQ(w["class"], "Button");
      EXPECT_EQ(w["label"], "OK");
    }
  }
  EXPECT_TRUE(found_button);
}

TEST_F(BuildTreeSnapshotTest, NoHitTestEntryYieldsNullRectAndFalseFlags) {
  hit_test_map hits; // empty: nothing has ever been rendered
  auto j = nlohmann::json::parse(build_tree_snapshot(1, "", tree_, hits));

  for (auto& w : j["widgets"]) {
    if (w["path"] == "ok") {
      EXPECT_TRUE(w["rect"].is_null());
      EXPECT_FALSE(w["hovered"]);
      EXPECT_FALSE(w["active"]);
      EXPECT_FALSE(w["visible"]);
    }
  }
}

TEST_F(BuildTreeSnapshotTest, JoinsHitTestEntryByWishId) {
  auto id = tree_["ok"]->get_as<bdg::bison::key_t>("__wish_id"_key, bdg::bison::key_t{});
  ASSERT_NE(id.id, 0u) << "import_json/ui_template assigns a nonzero __wish_id";

  hit_test_map hits;
  hits[id] = hit_test_entry{10.0f, 20.0f, 110.0f, 45.0f, true, false, true};

  auto j = nlohmann::json::parse(build_tree_snapshot(1, "", tree_, hits));
  for (auto& w : j["widgets"]) {
    if (w["path"] == "ok") {
      EXPECT_FLOAT_EQ(w["rect"]["x0"].get<float>(), 10.0f);
      EXPECT_FLOAT_EQ(w["rect"]["y1"].get<float>(), 45.0f);
      EXPECT_TRUE(w["hovered"]);
      EXPECT_FALSE(w["active"]);
      EXPECT_TRUE(w["visible"]);
    }
  }
}

TEST_F(BuildTreeSnapshotTest, RootFilterRestrictsToNodeAndDescendants) {
  hit_test_map hits;
  auto j = nlohmann::json::parse(build_tree_snapshot(1, "ok", tree_, hits));
  ASSERT_EQ(j["widgets"].size(), 1u);
  EXPECT_EQ(j["widgets"][0]["path"], "ok");
}

TEST_F(BuildTreeSnapshotTest, UnknownClassFallsBackToHexHash) {
  // A hand-built element with no registered class (never went through
  // dynamic::addClass()) has no DisplayName to resolve.
  bdg::wish::ui_tree solo;
  solo[""] = bdg::wish::ui_element_ptr{dynamic::instantiate<bdg::wish::ui_element>("NotRegistered"_key)};
  hit_test_map hits;
  auto j = nlohmann::json::parse(build_tree_snapshot(1, "", solo, hits));
  ASSERT_EQ(j["widgets"].size(), 1u);
  EXPECT_EQ(j["widgets"][0]["class"].get<std::string>().substr(0, 2), "0x");
}

// ── decode_query_tree_message() / encode_tree_snapshot() envelope codec ────

namespace {
void push_u8(std::vector<std::byte>& buf, uint8_t v) {
  buf.push_back(std::byte{v});
}
void push_u32(std::vector<std::byte>& buf, uint32_t v) {
  std::byte b[4];
  std::memcpy(b, &v, sizeof(v));
  buf.insert(buf.end(), b, b + sizeof(b));
}
std::vector<std::byte> build_envelope(bdg::wish::web_msg_type type, const std::string& payload) {
  std::vector<std::byte> out;
  push_u8(out, static_cast<uint8_t>(type));
  push_u8(out, 0);
  push_u8(out, 0);
  push_u8(out, 0);
  push_u32(out, static_cast<uint32_t>(payload.size()));
  auto p = reinterpret_cast<const std::byte*>(payload.data());
  out.insert(out.end(), p, p + payload.size());
  return out;
}
} // namespace

TEST(DrawProtocolAutomationTest, DecodeQueryTreeMessage_ReturnsRawJsonPayload) {
  auto msg = build_envelope(bdg::wish::web_msg_type::query_tree, R"({"request_id":1,"root":""})");
  auto payload = decode_query_tree_message(msg);
  ASSERT_TRUE(payload.has_value());
  EXPECT_EQ(*payload, R"({"request_id":1,"root":""})");
}

TEST(DrawProtocolAutomationTest, DecodeQueryTreeMessage_RejectsWrongMsgType) {
  auto msg = build_envelope(bdg::wish::web_msg_type::input, R"({"request_id":1})");
  EXPECT_FALSE(decode_query_tree_message(msg).has_value());
}

TEST(DrawProtocolAutomationTest, EncodeTreeSnapshot_WrapsJsonVerbatim) {
  std::string json = R"({"request_id":1,"widgets":[]})";
  auto bytes = encode_tree_snapshot(json);
  ASSERT_EQ(bytes.size(), 8 + json.size());
  EXPECT_EQ(static_cast<uint8_t>(bytes[0]), static_cast<uint8_t>(bdg::wish::web_msg_type::tree_snapshot));
  std::string round_tripped(reinterpret_cast<const char*>(bytes.data() + 8), json.size());
  EXPECT_EQ(round_tripped, json);
}

// ── end-to-end: QUERY_TREE -> TREE_SNAPSHOT over a real WebSocket ──────────
//
// Deliberately hand-rolled (not reusing test_web_renderer.cpp's private
// helpers) -- see that file's own rationale: tests should catch drift
// against the *documented* wire format, not just against the encoder's own
// internal logic.

namespace {

#if defined(_WIN32)
using raw_socket_t = SOCKET;
constexpr raw_socket_t kInvalidSocket = INVALID_SOCKET;
void close_socket(raw_socket_t sock) {
  ::closesocket(sock);
}
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
#else
using raw_socket_t = int;
constexpr raw_socket_t kInvalidSocket = -1;
void close_socket(raw_socket_t sock) {
  ::close(sock);
}
#endif

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

struct recv_result {
  uint8_t msg_type;
  std::string payload;
};

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
  uint32_t payload_len;
  std::memcpy(&payload_len, body.data() + 4, sizeof(payload_len));
  if (body.size() - 8 != payload_len)
    return std::nullopt;
  recv_result result;
  result.msg_type = body[0];
  result.payload.assign(reinterpret_cast<const char*>(body.data() + 8), payload_len);
  return result;
}

// Reads frames (skipping FRAME/TEX_* broadcasts, e.g. the font atlas) until
// a TREE_SNAPSHOT arrives, or @p max_frames is exhausted.
std::optional<recv_result> recv_tree_snapshot(int sock, int max_frames = 30) {
  for (int i = 0; i < max_frames; ++i) {
    auto r = recv_ws_frame(sock);
    if (!r)
      return std::nullopt;
    if (r->msg_type == static_cast<uint8_t>(bdg::wish::web_msg_type::tree_snapshot))
      return r;
  }
  return std::nullopt;
}

} // namespace

class WebRendererAutomationTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    bdg::wish::register_all();
  }

  void SetUp() override {
    renderer_ = std::make_unique<web_renderer>("127.0.0.1", 0, 16);
    renderer_->setup();
  }

  void TearDown() override {
    renderer_->teardown();
  }

  std::unique_ptr<web_renderer> renderer_;
};

TEST_F(WebRendererAutomationTest, QueryTreeRoundTripsThroughRealSocket) {
  int port = renderer_->actual_port();
  ASSERT_GT(port, 0);
  int sock = connect_ws_client(port);
  ASSERT_GE(sock, 0);

  // Drain the connect-triggered activity before sending QUERY_TREE, so the
  // wait loop below reacts to the query itself, not the earlier connect
  // (mirrors test_web_renderer.cpp's BeginFrame_DrainsQueuedMouseMoveIntoImGuiIO).
  for (int i = 0; i < 200; ++i) {
    if (renderer_->poll_events())
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  context ctx{"automation_test"_key};
  auto tree = bdg::wish::import_json(R"({"type":"Window","title":"T",
      "children":{"ok":{"type":"Button","label":"OK"}}})");
  assign_wish_ids(tree);
  ctx.ui_objects = std::move(tree);

  // Render one real frame so web_renderer::render_node() populates
  // hit_test_map_ for the Button.
  renderer_->begin_frame();
  renderer_->render_node(*ctx.ui_objects[""], ctx);
  renderer_->end_frame();

  send_ws_binary(sock, build_envelope(bdg::wish::web_msg_type::query_tree, R"({"request_id":42,"root":""})"));

  bool activity = false;
  for (int i = 0; i < 200 && !activity; ++i) {
    activity = renderer_->poll_events();
    if (!activity)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(activity);

  renderer_->service_automation_queries(ctx);

  auto snapshot = recv_tree_snapshot(sock);
  ASSERT_TRUE(snapshot.has_value());
  auto j = nlohmann::json::parse(snapshot->payload);
  EXPECT_EQ(j["request_id"], 42);

  bool found = false;
  for (auto& w : j["widgets"]) {
    if (w["path"] == "ok") {
      found = true;
      EXPECT_EQ(w["class"], "Button");
      EXPECT_EQ(w["label"], "OK");
      EXPECT_FALSE(w["rect"].is_null());
    }
  }
  EXPECT_TRUE(found);

  close_socket(sock);
}
