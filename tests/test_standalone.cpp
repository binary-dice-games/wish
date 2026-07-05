// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <standalone.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
namespace wish = bdg::wish;

// ── Test standalone subclass ──────────────────────────────────────────────────

class tracking_standalone : public wish::standalone {
 public:
  using wish::standalone::standalone;

  std::atomic<int> created_count{0};
  std::atomic<int> destroyed_count{0};
  wish::session* last_session{nullptr};
  std::function<void(bdg::bison::key_t, bdg::bison::key_t, dynamic)> emit_fn;

 protected:
  void on_session_created(wish::session& s) override {
    last_session = &s;
    emit_fn = s.emit_event;
    created_count.fetch_add(1, std::memory_order_release);
  }
  void on_session_destroyed(wish::session& s) override {
    (void)s;
    destroyed_count.fetch_add(1, std::memory_order_release);
  }
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TEST(StandaloneTest, StartStopDoesNotHang) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();
  sa.stop();
}

TEST(StandaloneTest, StartTriggersOnSessionCreatedExactlyOnce) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();
  EXPECT_EQ(sa.created_count.load(std::memory_order_acquire), 1);
  EXPECT_NE(sa.last_session, nullptr);
  sa.stop();
}

TEST(StandaloneTest, StopTriggersOnSessionDestroyedExactlyOnce) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();
  sa.stop();
  EXPECT_EQ(sa.destroyed_count.load(std::memory_order_acquire), 1);
}

// ── RMI operations (no transport, no serialization) ──────────────────────────

TEST(StandaloneTest, InstantiateWindowSucceeds) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto proxy = sa.instantiate("wish"_key, "Window"_key).get();
  EXPECT_TRUE(proxy.valid());
  EXPECT_NE(proxy.id(), 0u);

  sa.stop();
}

TEST(StandaloneTest, SetAppliesFieldValue) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto proxy = sa.instantiate("wish"_key, "Window"_key).get();

  dynamic fields;
  fields["title"_key] = std::string{"Hello"};
  bool ok = proxy.set(std::move(fields)).get();
  EXPECT_TRUE(ok);

  auto snapshot = proxy.get().get();
  EXPECT_EQ(snapshot.as<std::string>("title"_key), "Hello");

  sa.stop();
}

// ── upload_file / download_file ───────────────────────────────────────────────

TEST(StandaloneTest, UploadDownloadRoundTrips) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  sa.upload_file("hello.txt", "world").get();
  auto downloaded = sa.download_file("hello.txt").get();
  EXPECT_EQ(downloaded, "world");

  sa.stop();
}

// ── Event round-trip ──────────────────────────────────────────────────────────

TEST(StandaloneTest, EventFiresSynchronouslyInProcess) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto proxy = sa.instantiate("wish"_key, "Button"_key).get();

  std::atomic<bool> fired{false};
  proxy.onEvent("clicked"_key, [&fired](dynamic) { fired.store(true, std::memory_order_release); });

  ASSERT_TRUE(static_cast<bool>(sa.emit_fn));
  sa.emit_fn(proxy.id(), "clicked"_key, dynamic{});

  auto t0 = std::chrono::steady_clock::now();
  while (!fired.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  EXPECT_TRUE(fired.load(std::memory_order_acquire));

  sa.stop();
}
