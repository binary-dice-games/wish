// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <standalone/standalone.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
namespace wish = bdg::wish;

// ── Test standalone subclass ──────────────────────────────────────────────────

class tracking_standalone : public wish::standalone {
 public:
  using wish::standalone::standalone;

  std::atomic<int> created_count{0};
  std::atomic<int> destroyed_count{0};
  wish::context* last_session{nullptr};
  std::function<void(bdg::bison::key_t, bdg::bison::key_t, dynamic)> emit_fn;

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    emit_fn = s.emit_event;
    created_count.fetch_add(1, std::memory_order_release);
  }
  void on_session_destroyed(wish::context& s) override {
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

TEST(StandaloneTest, ChunkedUploadDownloadReportsProgress) {
  tracking_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  std::string data(1u << 21, 'x'); // 2 MiB, spans multiple 1 MiB chunks.

  std::vector<std::pair<std::uint64_t, std::uint64_t>> upload_progress;
  sa.upload_file(
        "big.bin", data,
        [&upload_progress](std::uint64_t transferred, std::uint64_t total) {
          upload_progress.emplace_back(transferred, total);
        })
      .get();
  ASSERT_FALSE(upload_progress.empty());
  EXPECT_GT(upload_progress.size(), 1u);
  for (std::size_t i = 1; i < upload_progress.size(); ++i)
    EXPECT_GT(upload_progress[i].first, upload_progress[i - 1].first);
  EXPECT_EQ(upload_progress.back().first, data.size());
  EXPECT_EQ(upload_progress.back().second, data.size());

  std::vector<std::pair<std::uint64_t, std::uint64_t>> download_progress;
  auto downloaded = sa.download_file(
                           "big.bin",
                           [&download_progress](std::uint64_t transferred, std::uint64_t total) {
                             download_progress.emplace_back(transferred, total);
                           })
                         .get();
  EXPECT_EQ(downloaded, data);
  ASSERT_FALSE(download_progress.empty());
  EXPECT_GT(download_progress.size(), 1u);
  for (std::size_t i = 1; i < download_progress.size(); ++i)
    EXPECT_GT(download_progress[i].first, download_progress[i - 1].first);
  EXPECT_EQ(download_progress.back().first, data.size());
  EXPECT_EQ(download_progress.back().second, data.size());

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

// ── Context factory hook (on_create_context) ──────────────────────────────────

namespace {

class custom_context : public wish::context {
 public:
  using wish::context::context;
  bool marker = true;
};

class custom_context_standalone : public wish::standalone {
 public:
  using wish::standalone::standalone;

  wish::context* last_session{nullptr};

 protected:
  std::unique_ptr<bdg::bison::rmi::context> on_create_context(bdg::bison::key_t session_id) override {
    return std::make_unique<custom_context>(session_id);
  }
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

} // namespace

TEST(StandaloneTest, CustomContextFactoryIsUsed) {
  custom_context_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();
  ASSERT_NE(sa.last_session, nullptr);
  EXPECT_NE(dynamic_cast<custom_context*>(sa.last_session), nullptr);
  sa.stop();
}

// ── on_create_object extensibility ─────────────────────────────────────────────

namespace {

class custom_object_standalone : public wish::standalone {
 public:
  using wish::standalone::standalone;

  int window_dispatch_count{0};

 protected:
  bdg::bison::dynamic_ptr on_create_object(
      bdg::bison::rmi::context& ctx, bdg::bison::key_t ns, bdg::bison::key_t klass) override {
    if (klass == "Window"_key)
      ++window_dispatch_count;
    // Falls back to the base implementation for everything -- this only
    // compiles/links if on_create_object is protected (not private) and
    // overridable (not final), proving Part A's un-sealing.
    return wish::standalone::on_create_object(ctx, ns, klass);
  }
};

} // namespace

TEST(StandaloneTest, CustomOnCreateObjectCanObserveAndFallsBackToBase) {
  custom_object_standalone sa{std::make_unique<wish::null_renderer>()};
  sa.start();

  auto window_proxy = sa.instantiate("wish"_key, "Window"_key).get();
  EXPECT_TRUE(window_proxy.valid());
  EXPECT_EQ(sa.window_dispatch_count, 1);

  // An unrelated class still dispatches correctly through the fallback path.
  auto button_proxy = sa.instantiate("wish"_key, "Button"_key).get();
  EXPECT_TRUE(button_proxy.valid());
  EXPECT_EQ(sa.window_dispatch_count, 1);

  sa.stop();
}
