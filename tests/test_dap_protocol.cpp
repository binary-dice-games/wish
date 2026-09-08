// MIT License © 2026 Binary Dice Games
/// @file test_dap_protocol.cpp
/// @brief Pure unit tests for the Debug Adapter Protocol framing helpers
///        (modules/bdg/dev/dbg/client/dap_protocol.hpp) that
///        python_debug_backend depends on. No sockets, no subprocess — runs
///        on every platform (the counterpart of test_mi_parser.cpp for the
///        GDB/MI backend).
#include "modules/bdg/dev/dbg/client/dap_protocol.hpp"

#include <gtest/gtest.h>

#include <string>

namespace bdg::wish::dbg {
namespace {

std::string wire(const std::string& body) {
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

TEST(DapProtocol, ReadsOneCompleteMessage) {
  dap_message_reader r;
  r.feed(wire(R"({"seq":1,"type":"event","event":"initialized"})"));
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["type"], "event");
  EXPECT_EQ((*msg)["event"], "initialized");
  EXPECT_FALSE(r.next());
}

TEST(DapProtocol, ReassemblesAcrossChunkBoundaries) {
  const std::string body = R"({"seq":7,"type":"response","request_seq":3,"success":true,"command":"threads"})";
  const std::string framed = wire(body);

  dap_message_reader r;
  // Feed one byte at a time — header and body both split arbitrarily.
  for (char c : framed) {
    r.feed(std::string_view(&c, 1));
  }
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["request_seq"], 3);
  EXPECT_EQ((*msg)["command"], "threads");
}

TEST(DapProtocol, ReadsBackToBackMessagesInOneBuffer) {
  dap_message_reader r;
  r.feed(wire(R"({"type":"event","event":"a"})") + wire(R"({"type":"event","event":"b"})") +
         wire(R"({"type":"event","event":"c"})"));

  auto a = r.next();
  auto b = r.next();
  auto c = r.next();
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ((*a)["event"], "a");
  EXPECT_EQ((*b)["event"], "b");
  EXPECT_EQ((*c)["event"], "c");
  EXPECT_FALSE(r.next());
}

TEST(DapProtocol, WaitsForFullBodyBeforeYielding) {
  const std::string body = R"({"type":"event"})"; // 16 bytes
  dap_message_reader r;
  r.feed("Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body.substr(0, 9));
  EXPECT_FALSE(r.next());
  r.feed(body.substr(9)); // completes the body
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["type"], "event");
}

TEST(DapProtocol, SkipsHeaderWithNoContentLength) {
  dap_message_reader r;
  r.feed("X-Bogus: 1\r\n\r\n"); // unusable header, no body count
  r.feed(wire(R"({"type":"event","event":"ok"})"));
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["event"], "ok");
}

TEST(DapProtocol, CaseInsensitiveHeaderName) {
  dap_message_reader r;
  const std::string body = R"({"type":"event","event":"x"})";
  r.feed("content-length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["event"], "x");
}

TEST(DapProtocol, MalformedJsonBodyIsSkipped) {
  dap_message_reader r;
  r.feed(wire("{not valid json"));
  r.feed(wire(R"({"type":"event","event":"recovered"})"));
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["event"], "recovered");
}

TEST(DapProtocol, FrameRoundTrips) {
  dap_json req = dap_make_request(42, "setBreakpoints", {{"source", {{"path", "/x.py"}}}});
  std::string framed = dap_frame(req);

  dap_message_reader r;
  r.feed(framed);
  auto msg = r.next();
  ASSERT_TRUE(msg);
  EXPECT_EQ((*msg)["seq"], 42);
  EXPECT_EQ((*msg)["type"], "request");
  EXPECT_EQ((*msg)["command"], "setBreakpoints");
  EXPECT_EQ((*msg)["arguments"]["source"]["path"], "/x.py");
}

TEST(DapProtocol, MakeRequestOmitsNullArguments) {
  dap_json req = dap_make_request(1, "configurationDone");
  EXPECT_FALSE(req.contains("arguments"));
}

} // namespace
} // namespace bdg::wish::dbg
