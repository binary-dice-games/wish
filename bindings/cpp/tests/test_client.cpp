// MIT License © 2025 Binary Dice Games
//
// Tests for the header-only C++ wish client binding. Unlike the native
// client's own tests (tests/test_client.cpp, in-process memory transport),
// these exercise the binding the way a real out-of-process consumer would:
// only wish_client_dll's C ABI on the client side. A real wish::server
// (linked from the full `wish` library, socket transport) is spun up
// in-process purely as the test fixture's counterpart -- the binding under
// test never sees anything but wish_client_c.h/bison_c.h/rmi_c.h.

#include <wish_cpp/wish.hpp>

#include <bison_c.h>
#include <gtest/gtest.h>
#include <server/registry.hpp>
#include <server/server.hpp>

#include "src/rmi/transport/socket_transport.hpp"
#include "src/rmi/transport/tls_socket_transport.hpp"
#include "tests/tls_test_certs.hpp"

#include <chrono>
#include <thread>

namespace wish = bdg::wish::binding;
using namespace bdg::wish::binding;  // for the "_key" literal operator

// ── key_t: constexpr literal must be bit-identical to the runtime C ABI ────

TEST(WishCppKeyTest, LiteralMatchesRuntimeWishKey) {
  for (const char* name : {"clicked", "text", "row0.c", "", "a_much_longer_field_name_here"}) {
    wish::hash_t runtime = wish_key(name);
    wish::hash_t compile_time = wish::key_t{name}.id;
    EXPECT_EQ(runtime, compile_time) << "mismatch for \"" << name << "\"";
  }
}

TEST(WishCppKeyTest, LiteralIsConstexpr) {
  constexpr wish::key_t k = "clicked"_key;
  static_assert(k.id != 0, "must hash to something non-zero");
  EXPECT_EQ(k.id, wish_key("clicked"));
}

TEST(WishCppKeyTest, MsbIsAlwaysSet) {
  EXPECT_NE("x"_key.id & 0x80000000u, 0u);
}

// ── value: header-only bison_handle wrapper ────────────────────────────────

TEST(WishCppValueTest, ScalarFieldsRoundTrip) {
  wish::value v;
  v.set_int("i"_key, 42).set_float("f"_key, 1.5f).set_bool("b"_key, true).set_string("s"_key, "hi");

  EXPECT_EQ(v.get_int("i"_key), 42);
  EXPECT_FLOAT_EQ(*v.get_float("f"_key), 1.5f);
  EXPECT_EQ(v.get_bool("b"_key), true);
  EXPECT_EQ(v.get_string("s"_key), "hi");

  // bison_get_int() auto-vivifies an absent field as its type's default (0)
  // rather than reporting BISON_ERR_NOT_FOUND -- same behavior as
  // bison::dynamic::operator[]() at the C++ level.
  EXPECT_EQ(v.get_int("missing"_key), 0);
}

TEST(WishCppValueTest, FieldRefAssignmentSugar) {
  wish::value v;
  v["text"_key] = std::string{"hello"};
  v["count"_key] = 7;
  EXPECT_EQ(v.get_string("text"_key), "hello");
  EXPECT_EQ(v.get_int("count"_key), 7);
}

TEST(WishCppValueTest, NestedObjectRoundTrips) {
  wish::value inner;
  inner["x"_key] = 1;
  wish::value outer;
  outer["child"_key] = inner;

  auto child = outer.get_object("child"_key);
  ASSERT_TRUE(child.has_value());
  EXPECT_EQ(child->get_int("x"_key), 1);
}

TEST(WishCppValueTest, JsonRoundTrips) {
  auto v = wish::value::parse_json(R"({"a":1,"b":"two"})");
  EXPECT_EQ(v.get_int("a"_key), 1);
  EXPECT_EQ(v.get_string("b"_key), "two");
}

TEST(WishCppValueTest, ArrayIndexRoundTrips) {
  auto v = wish::value::parse_json(R"({"items":["a","b","c"]})");
  auto items = v.get_object("items"_key);
  ASSERT_TRUE(items.has_value());
  ASSERT_EQ(items->size(), 3u);
  EXPECT_EQ(items->get_string_at(0), "a");
  EXPECT_EQ(items->get_string_at(2), "c");
}

// ── client / proxy: end-to-end against a real wish::server ────────────────

namespace {

constexpr uint16_t kTestPort = 17071;
constexpr const char* kWindowDesc = R"({
  "type": "Window",
  "title": "T",
  "children": { "label": { "type": "Label", "text": "hi" } }
})";

class WishCppClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_unique<bdg::bison::rmi::transport::socket_server_transport>("127.0.0.1", kTestPort);
    server_ = std::make_unique<bdg::wish::server>(*transport_, std::make_unique<bdg::wish::null_renderer>());
    server_->start();
  }

  void TearDown() override { server_->stop(); }

  std::unique_ptr<bdg::bison::rmi::transport::socket_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
};

}  // namespace

TEST_F(WishCppClientTest, RegisterInstantiateAndSetGetRoundTrip) {
  auto client = wish::client::tcp("127.0.0.1", kTestPort);
  client.run([](wish::client& c) {
    c.register_template("win", kWindowDesc);
    auto root = c.instantiate_template("win", "win");
    EXPECT_TRUE(root.valid());

    auto label = c.proxy_get("win.label");
    EXPECT_EQ(*label.get().get_string("text"_key), "hi");

    wish::value f;
    f["text"_key] = std::string{"updated"};
    label.set(f);
    EXPECT_EQ(*label.get().get_string("text"_key), "updated");
  });
}

TEST_F(WishCppClientTest, ProxyGetForUnknownPathThrows) {
  auto client = wish::client::tcp("127.0.0.1", kTestPort);
  client.run([](wish::client& c) { EXPECT_THROW(c.proxy_get("no.such.path"), wish::error); });
}

// Firing a real "clicked" event needs a live renderer frame loop (see
// src/context/context.hpp's emit_event() / pending_events plumbing) --
// beyond what a headless null_renderer test fixture can drive. This test
// covers the C ABI subscription round-trip only (rmi_proxy_on_event()
// succeeding, callback pointer surviving); the calculator/notepad examples
// under bindings/cpp/examples/ exercise real event delivery end-to-end
// against a rendering server.
TEST_F(WishCppClientTest, OnEventSubscriptionSucceeds) {
  auto client = wish::client::tcp("127.0.0.1", kTestPort);
  client.run([](wish::client& c) {
    c.register_template(
        "btnwin", R"({"type":"Window","title":"T","children":{"btn":{"type":"Button","label":"go"}}})");
    auto root = c.instantiate_template("btnwin", "btnwin");
    auto btn = c.proxy_get("btnwin.btn");

    EXPECT_NO_THROW(btn.on_event("clicked"_key, [](wish::value) {}));
  });
}

// ── client / proxy: end-to-end against a real wish::server over TLS ────────
//
// Exercises wish_client_tls_create() (the C ABI factory added alongside
// bison's tls_socket_client_transport) through the header-only binding's
// client::tls(), against a real tls_socket_server_transport-backed
// wish::server -- the same shape as WishCppClientTest above, but over an
// encrypted, server-authenticated connection.

namespace {

constexpr uint16_t kTlsTestPort = 17075;

class WishCppTlsClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    transport_ = std::make_unique<bdg::bison::rmi::transport::tls_socket_server_transport>(
        "127.0.0.1", kTlsTestPort);
    server_ = std::make_unique<bdg::wish::server>(*transport_, std::make_unique<bdg::wish::null_renderer>());
    server_->start(nullptr,
        bdg::bison::rmi::transport::test::tls_server_params(
            bdg::bison::rmi::transport::test::kTestServerCert, bdg::bison::rmi::transport::test::kTestServerKey));
  }

  void TearDown() override { server_->stop(); }

  std::unique_ptr<bdg::bison::rmi::transport::tls_socket_server_transport> transport_;
  std::unique_ptr<bdg::wish::server> server_;
};

}  // namespace

TEST_F(WishCppTlsClientTest, RegisterInstantiateAndSetGetRoundTrip) {
  auto client = wish::client::tls("127.0.0.1", kTlsTestPort);
  wish::value connect_params;
  connect_params["ca_pem"_key] = bdg::bison::rmi::transport::test::kTestCaCert;
  client.run(
      [](wish::client& c) {
        c.register_template("win", kWindowDesc);
        auto root = c.instantiate_template("win", "win");
        EXPECT_TRUE(root.valid());

        auto label = c.proxy_get("win.label");
        EXPECT_EQ(*label.get().get_string("text"_key), "hi");
      },
      connect_params);
}

TEST_F(WishCppTlsClientTest, MissingTrustAnchorFailsHandshake) {
  // No ca_pem supplied and insecure_skip_verify left false -- the client has
  // no trust anchor for the server's certificate, so the handshake inside
  // run()'s connect step must fail rather than silently succeeding.
  auto client = wish::client::tls("127.0.0.1", kTlsTestPort);
  EXPECT_THROW(client.run([](wish::client&) {}), wish::error);
}
