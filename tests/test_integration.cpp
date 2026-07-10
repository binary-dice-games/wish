// MIT License © 2025 Binary Dice Games
//
// End-to-end integration test: exercises the full stack over an in-memory
// transport — template registration, property set/get, event delivery, file
// operations, and session cleanup after disconnect.
#include <gtest/gtest.h>

#include <client/client.hpp>
#include <server/server.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Integration server ────────────────────────────────────────────────────────

// Subclass that captures per-session state needed for server-side assertions
// and for triggering events from outside the render loop.
class integration_server : public wish::server {
 public:
  integration_server(memory_server_transport& t) : wish::server(t, std::make_unique<wish::null_renderer>()) {}

  // Callback wired to ctx.emit_event; available once on_session_created fires.
  std::function<void(bdg::bison::key_t, bdg::bison::key_t, dynamic)> emit_fn;

  // Sandboxed resource directory for the active session.
  std::filesystem::path resource_dir;

  // Number of live top_level_objects entries for the (single) active
  // session -- the render loop (server.cpp) iterates this map every frame,
  // so a stale entry left behind after a template root is destroyed would
  // otherwise be rendered forever. Assumes exactly one connected session.
  size_t top_level_object_count() {
    auto lp = session_contexts().rlock();
    for (auto& [id, holder] : *lp) {
      auto clp = holder->rlock();
      return static_cast<wish::context&>(**clp).top_level_objects.size();
    }
    return 0;
  }

 protected:
  void on_session_created(wish::context& s) override {
    emit_fn = s.emit_event;
    resource_dir = s.resource_dir;
  }
};

// ── Fixture ───────────────────────────────────────────────────────────────────

class IntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<integration_server>(transport_);
    srv_->start();
  }

  void TearDown() override {
    srv_->stop();
    srv_.reset();
  }

  memory_server_transport transport_;
  std::unique_ptr<integration_server> srv_;
};

// ── Integration descriptor ────────────────────────────────────────────────────

static constexpr const char* kUIDesc = R"({
  "type": "Window",
  "title": "Integration",
  "children": {
    "layout": {
      "type": "VerticalLayout",
      "children": {
        "lbl": { "type": "Label",  "text":  "initial" },
        "btn": { "type": "Button", "label": "Go"      }
      }
    }
  }
})";

// ── Test ──────────────────────────────────────────────────────────────────────

TEST_F(IntegrationTest, FullStack) {
  // Results are stored as members so the outer TEST_F body can assert on them
  // after client.run() returns.
  class test_client : public wish::client {
   public:
    using wish::client::client;

    integration_server* srv{nullptr};

    // Per-phase outcome flags checked outside on_session().
    bool lbl_set_ok{false};
    bool event_ok{false};
    bool file_upload_ok{false};
    bool file_list_ok{false};
    bool file_download_ok{false};
    bool file_erase_ok{false};

   protected:
    void on_session() override {
      // ── 1. Template: register + instantiate ──────────────────────────────
      register_template("ui"_key, wish::import_descriptor_json(kUIDesc)).get();
      auto pm = instantiate_template("ui"_key).get();

      ASSERT_TRUE(pm.count("")) << "root proxy missing";
      ASSERT_TRUE(pm.count("layout.lbl")) << "label proxy missing";
      ASSERT_TRUE(pm.count("layout.btn")) << "button proxy missing";

      // ── 2. Property set + server-side get verification ───────────────────
      {
        dynamic f;
        f["text"_key] = std::string{"Hello"};
        pm.at("layout.lbl").set(std::move(f)).get();

        auto snap = pm.at("layout.lbl").get().get();
        lbl_set_ok = (snap.as<std::string>("text"_key) == "Hello");
      }

      // ── 3. Event: register handler → trigger via server emit_event ───────
      {
        std::atomic<bool> fired{false};
        pm.at("layout.btn").onEvent("clicked"_key, [&fired](dynamic) { fired.store(true, std::memory_order_release); });

        // Simulate a button click from the server side.
        srv->emit_fn(pm.at("layout.btn").object_id(), "clicked"_key, dynamic{});

        // Spin until the client worker thread delivers the event (≤ 2 s).
        auto t0 = std::chrono::steady_clock::now();
        while (!fired.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
          std::this_thread::sleep_for(std::chrono::milliseconds(5));

        event_ok = fired.load(std::memory_order_acquire);
      }

      // ── 4. File operations ────────────────────────────────────────────────
      {
        // Upload
        upload_file("test.bin", "integration-data").get();
        file_upload_ok = true;

        // List via raw proxy (no dedicated helper in wish::client)
        auto fs = instantiate("wish"_key, "__WishFileSystem"_key).get();
        {
          auto result = fs.call("list"_key, dynamic{}).get();
          result.forEach([this](bdg::bison::key_t, const field& f) {
            if (f.is<std::string>() && f.as<std::string>() == "test.bin")
              file_list_ok = true;
          });
        }

        // Download and verify content
        {
          auto content = download_file("test.bin").get();
          file_download_ok = (content == "integration-data");
        }

        // Delete the file
        {
          dynamic args;
          args["name"_key] = std::string{"test.bin"};
          fs.call("erase"_key, std::move(args)).get();
          file_erase_ok = true;
        }
      }
    } // on_session()
  };

  // ── Run the client ────────────────────────────────────────────────────────
  test_client c{transport_.connect()};
  c.srv = srv_.get();
  c.run(); // connect → on_session() → disconnect

  // ── Assertions ────────────────────────────────────────────────────────────
  EXPECT_TRUE(c.lbl_set_ok) << "label text set/get round-trip failed";
  EXPECT_TRUE(c.event_ok) << "clicked event was not delivered";
  EXPECT_TRUE(c.file_upload_ok) << "file upload failed";
  EXPECT_TRUE(c.file_list_ok) << "file not found in list result";
  EXPECT_TRUE(c.file_download_ok) << "file download content mismatch";
  EXPECT_TRUE(c.file_erase_ok) << "file erase call failed";

  // Session resource_dir must be deleted when the client disconnects. The
  // client's disconnect() does not block on the server finishing its own
  // teardown (context destruction happens on the server's worker thread,
  // after it notices the connection closed) -- spin briefly, same as the
  // event-delivery wait above.
  ASSERT_FALSE(srv_->resource_dir.empty()) << "server did not record resource_dir";
  {
    auto t0 = std::chrono::steady_clock::now();
    while (std::filesystem::exists(srv_->resource_dir) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_FALSE(std::filesystem::exists(srv_->resource_dir)) << "resource_dir still exists after disconnect";
}

// ── Template root destroy must remove it from the render loop ─────────────────
//
// server.cpp's render loop iterates context::top_level_objects
// unconditionally every frame. wish::form removes its own internal window
// from that map when destroyed (form::remove_internal_objects(), wired into
// ~form()); a raw register_template/instantiate_template root previously had
// no equivalent cleanup, so destroying it over RMI left a dangling entry the
// renderer kept drawing forever (see ui_template.cpp's instantiate_prototype
// HOOK_DESTRUCT).

TEST_F(IntegrationTest, DestroyingTemplateRootRemovesTopLevelObject) {
  class test_client : public wish::client {
   public:
    using wish::client::client;

    integration_server* srv{nullptr};
    size_t count_before_destroy{static_cast<size_t>(-1)};
    size_t count_after_destroy{static_cast<size_t>(-1)};

   protected:
    void on_session() override {
      register_template("ui"_key, wish::import_descriptor_json(kUIDesc)).get();
      auto pm = instantiate_template("ui"_key).get();
      ASSERT_TRUE(pm.count("")) << "root proxy missing";

      count_before_destroy = srv->top_level_object_count();

      pm.at("").destroy();

      count_after_destroy = srv->top_level_object_count();
    }
  };

  test_client c{transport_.connect()};
  c.srv = srv_.get();
  c.run();

  EXPECT_GT(c.count_before_destroy, c.count_after_destroy)
      << "expected destroying the template root to remove its top_level_objects entry";
  EXPECT_EQ(c.count_after_destroy, 0u) << "expected no top-level objects left after destroying the only template";
}
