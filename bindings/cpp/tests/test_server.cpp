// MIT License © 2025 Binary Dice Games
//
// Tests for the header-only C++ wish server binding (server.hpp), built
// entirely on wish_server_c.h/bison_c.h through the prebuilt wish_server_dll
// shared library -- mirrors bindings/python/tests/test_server.py's
// lifecycle-only coverage. Deliberately does NOT also link wish_client_dll
// in this binary (or use wish_cpp::client) -- see server.hpp's file doc
// comment for why that would make bison_handle resolution link-order
// dependent; full round-trip coverage of the underlying term/TLS transports
// lives at the C ABI level (src/wish_server_c.cpp) and in the Python
// binding's tests.

#include <wish_cpp/server.hpp>

#include <gtest/gtest.h>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator

constexpr uint16_t kTestPort = 17081;
constexpr uint16_t kTestTlsPort = 17082;

TEST(WishCppServerLifecycleTest, TcpCreateAndDestroy) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  EXPECT_NE(server.handle(), nullptr);
}

TEST(WishCppServerLifecycleTest, TlsCreateAndDestroy) {
  auto server = wish::server::tls("127.0.0.1", kTestTlsPort);
  EXPECT_NE(server.handle(), nullptr);
}

TEST(WishCppServerLifecycleTest, TermCreateAndDestroy) {
  // "true" exits immediately once spawned -- exercises spawn/teardown
  // without leaving a shell process attached to the test run.
  auto server = wish::server::term("/bin/true");
  EXPECT_NE(server.handle(), nullptr);
}

TEST(WishCppServerLifecycleTest, StopBeforeStartIsNoop) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  EXPECT_NO_THROW(server.stop());
}

TEST(WishCppServerLifecycleTest, ShouldQuitFalseBeforeStart) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  EXPECT_FALSE(server.should_quit());
}

TEST(WishCppServerLifecycleTest, StartWithRendererParams) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  wish::value params;
  params["title"_key] = std::string{"Custom Title"};
  params["width"_key] = int32_t{800};
  params["height"_key] = int32_t{600};
  EXPECT_NO_THROW(server.start("console", params));
  server.stop();
}

TEST(WishCppServerLifecycleTest, BadRendererKindThrows) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  try {
    server.start("not-a-real-renderer");
    FAIL() << "expected wish::error";
  } catch (const wish::error& e) {
    EXPECT_EQ(e.code(), WISH_SERVER_ERR_BAD_RENDERER);
  }
}

TEST(WishCppServerLifecycleTest, MoveConstructionTransfersOwnership) {
  auto server = wish::server::tcp("127.0.0.1", kTestPort);
  wish_server_handle h = server.handle();
  wish::server moved(std::move(server));
  EXPECT_EQ(moved.handle(), h);
  EXPECT_EQ(server.handle(), nullptr);  // NOLINT(bugprone-use-after-move)
}
