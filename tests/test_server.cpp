// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/rmi/rmi.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Test server subclasses ────────────────────────────────────────────────────

class tracking_server : public wish::server {
 public:
  tracking_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r) : wish::server(t, std::move(r)) {}

  std::atomic<int> created_count{0};
  std::atomic<int> destroyed_count{0};
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    created_count.fetch_add(1, std::memory_order_release);
  }
  void on_session_destroyed(wish::context& s) override {
    (void)s;
    destroyed_count.fetch_add(1, std::memory_order_release);
  }
};

class multi_tracking_server : public wish::server {
 public:
  multi_tracking_server(server_transport_iface& t, std::unique_ptr<wish::renderer> r) : wish::server(t, std::move(r)) {}

  std::atomic<int> created_count{0};
  std::mutex sessions_mutex;
  std::vector<wish::context*> sessions;

 protected:
  void on_session_created(wish::context& s) override {
    std::lock_guard<std::mutex> lk(sessions_mutex);
    sessions.push_back(&s);
    created_count.fetch_add(1, std::memory_order_release);
  }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

// ── Lifecycle ─────────────────────────────────────────────────────────────────

TEST(ServerTest, StartStopDoesNotHang) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();
  srv.stop();
}

// ── Session lifecycle ─────────────────────────────────────────────────────────

TEST(ServerTest, ClientConnectTriggersOnSessionCreated) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();
    // on_session_created fires during accept() — before connect() returns
    EXPECT_EQ(srv.created_count.load(std::memory_order_acquire), 1);
    EXPECT_NE(srv.last_session, nullptr);
    c.disconnect();
  }

  srv.stop();
}

TEST(ServerTest, ClientDisconnectTriggersOnSessionDestroyed) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();
    c.disconnect();
  }

  // stop() joins all bison workers; on_session_destroyed has fired by then
  srv.stop();
  EXPECT_EQ(srv.destroyed_count.load(std::memory_order_acquire), 1);
}

// ── RMI operations ────────────────────────────────────────────────────────────

TEST(ServerTest, InstantiateWindowSucceeds) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();

    auto proxy = c.instantiate("wish"_key, "Window"_key).get();
    EXPECT_TRUE(proxy.valid());
    EXPECT_NE(proxy.id(), 0u);

    c.disconnect();
  }

  srv.stop();
}

TEST(ServerTest, SetAppliesFieldValue) {
  memory_server_transport transport;
  tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();

    auto proxy = c.instantiate("wish"_key, "Window"_key).get();

    dynamic fields;
    fields["title"_key] = std::string{"Hello"};
    bool ok = proxy.set(std::move(fields)).get();
    EXPECT_TRUE(ok);

    // Verify the field was actually applied by reading it back.
    auto snapshot = proxy.get().get();
    EXPECT_EQ(snapshot.as<std::string>("title"_key), "Hello");

    c.disconnect();
  }

  srv.stop();
}

// ── Multiple clients ──────────────────────────────────────────────────────────

TEST(ServerTest, TwoClientsGetSeparateSessions) {
  memory_server_transport transport;
  multi_tracking_server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    client c1{transport.connect()};
    client c2{transport.connect()};

    c1.connect();
    c2.connect();

    EXPECT_EQ(srv.created_count.load(std::memory_order_acquire), 2);

    {
      std::lock_guard<std::mutex> lk(srv.sessions_mutex);
      ASSERT_EQ(srv.sessions.size(), 2u);
      EXPECT_NE(srv.sessions[0], srv.sessions[1]);
      EXPECT_NE(srv.sessions[0]->session_id, srv.sessions[1]->session_id);
    }

    c1.disconnect();
    c2.disconnect();
  }

  srv.stop();
}

// ── MenuBarExtension splice / skip-double-render ────────────────────────────
//
// Covers the extensible-chrome design: a session's MenuBarExtension
// top-level object must be visible to render_server_frame() (so a host
// renderer can splice its children into the server's own menu bar) but must
// NOT also be drawn by the normal per-session render_session() path, or it
// would be double-rendered as a standalone window.

namespace {

// Records every render_server_frame()/render_session() call so a test can
// assert on which sessions/classes were seen by each path, without needing a
// real ImGui backend.
class recording_renderer : public wish::renderer {
 public:
  std::mutex mtx;
  std::condition_variable cv;
  int server_frame_calls = 0;
  size_t last_sessions_seen = 0;
  bool menu_bar_extension_seen_by_server_frame = false;
  std::vector<bdg::bison::key_t> render_session_classes;

  void begin_frame() override {}
  void end_frame() override {}

  void render_server_frame(const std::vector<wish::sync_context_ptr>& sessions) override {
    std::lock_guard<std::mutex> lk(mtx);
    ++server_frame_calls;
    last_sessions_seen = sessions.size();
    for (const auto& sync_ctx : sessions) {
      auto sess = wish::context_rlock{*sync_ctx};
      for (const auto& [key, win] : sess->top_level_objects) {
        if (win && win->as<bdg::bison::key_t>(dynamic::CLASS) == bdg::bison::key_t{"MenuBarExtension"})
          menu_bar_extension_seen_by_server_frame = true;
      }
    }
    cv.notify_all();
  }

  void render_node(const wish::ui_element& node, const wish::context& s) override {
    // render_session() (the base's default, un-overridden here) calls
    // render_node() once per top-level root; recurses into children for a
    // full tree walk, mirroring counting_renderer in test_renderer.cpp.
    std::lock_guard<std::mutex> lk(mtx);
    render_session_classes.push_back(node.as<bdg::bison::key_t>(dynamic::CLASS));
    wish::render_children(*this, node, s);
  }
};

// Registers and instantiates a one-node-root template (via __WishTemplate,
// the same RMI mechanism wish_desktop::build_chrome() uses) so its root
// lands in the session's top_level_objects.
void instantiate_template(client& c, const std::string& name, const std::string& descriptor_json) {
  auto tmpl = c.instantiate("wish"_key, "__WishTemplate"_key).get();

  dynamic reg_args;
  reg_args["name"_key] = bdg::bison::key_t{name};
  reg_args["descriptor"_key] = dynamic_ptr{wish::import_descriptor_json(descriptor_json)};
  tmpl.call("register"_key, std::move(reg_args)).get();

  dynamic inst_args;
  inst_args["name"_key] = bdg::bison::key_t{name};
  tmpl.call("instantiate"_key, std::move(inst_args)).get();
}

} // namespace

TEST(ServerTest, MenuBarExtensionSplicedNotDoubleRendered) {
  memory_server_transport transport;
  auto rec_owner = std::make_unique<recording_renderer>();
  recording_renderer& rec = *rec_owner;
  wish::server srv{transport, std::move(rec_owner)};
  srv.start();

  {
    client c{transport.connect()};
    c.connect();

    instantiate_template(c, "__test_menu_ext", R"json({
      "type": "MenuBarExtension",
      "children": { "m_file": { "type": "Menu", "label": "File" } }
    })json");
    instantiate_template(c, "__test_window", R"json({ "type": "Window", "title": "Plain" })json");

    // Wait for the render loop to observe both top-level objects and draw at
    // least one frame; render_loop() ticks every 5ms and only redraws when a
    // session is dirty (set by on_after_dispatch() after the calls above).
    {
      std::unique_lock<std::mutex> lk(rec.mtx);
      ASSERT_TRUE(rec.cv.wait_for(lk, std::chrono::seconds{2}, [&] {
        return rec.server_frame_calls > 0 && rec.menu_bar_extension_seen_by_server_frame &&
            !rec.render_session_classes.empty();
      }));
    }

    {
      std::lock_guard<std::mutex> lk(rec.mtx);
      // render_server_frame() must see the session so it can splice the
      // MenuBarExtension's children into the host menu bar.
      EXPECT_GT(rec.last_sessions_seen, size_t{0});
      EXPECT_TRUE(rec.menu_bar_extension_seen_by_server_frame);

      // The per-session render_session() path must have drawn the plain
      // Window root, but never the MenuBarExtension root -- it is only ever
      // reached via render_server_frame()'s splice, never as a standalone
      // top-level window.
      bool window_rendered = false;
      bool extension_rendered_standalone = false;
      for (const bdg::bison::key_t& cls : rec.render_session_classes) {
        if (cls == "Window"_key)
          window_rendered = true;
        if (cls == "MenuBarExtension"_key)
          extension_rendered_standalone = true;
      }
      EXPECT_TRUE(window_rendered);
      EXPECT_FALSE(extension_rendered_standalone);
    }

    c.disconnect();
  }

  srv.stop();
}
