// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <ui/dock_layout_spec.hpp>
#include <ui/forms/form.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/server/context.hpp"

#include <functional>
#include <memory>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace rmi = bdg::bison::rmi;

// ── stub_form ─────────────────────────────────────────────────────────────────

/// Minimal concrete form used throughout these tests.
class stub_form : public wish::form {
 public:
  explicit stub_form(dynamic&& base) : form(std::move(base)) {}

  bool init_called_{false};
  rmi::context* ctx_ref_{nullptr};
  wish::context* sess_ref_{nullptr};

  /// Expose emit() for testing (it is protected in form).
  void test_emit(bison::key_t event_name, dynamic payload = {}) {
    emit(event_name, std::move(payload));
  }

 protected:
  void on_init() override {
    init_called_ = true;
    ctx_ref_ = &ctx();
    sess_ref_ = &sess();
  }
};

// ── tracked_stub_form (defined here so ensure_registered() can reference it) ──

// Track which stub_form instances had init() called.
static std::atomic<int> g_server_init_count{0};

class tracked_stub_form : public wish::form {
 public:
  explicit tracked_stub_form(dynamic&& base) : form(std::move(base)) {}

 protected:
  void on_init() override {
    g_server_init_count.fetch_add(1);
  }
};

// ── Registration helper ───────────────────────────────────────────────────────

static void ensure_registered() {
  // Registers all classes needed by these tests; idempotent via static guard.
  static bool done = false;
  if (done)
    return;
  done = true;
  bdg::wish::register_all();
  auto proto = dynamic_ptr{"__StubForm"_key, {}};
  dynamic::addClass(
      "wish"_key, std::move(proto), bison::key_t{0U}, dynamic::make_factory<stub_form>("wish"_key, "__StubForm"_key));
  auto tracked_proto = dynamic_ptr{"__TrackedStubForm"_key, {}};
  dynamic::addClass(
      "wish"_key,
      std::move(tracked_proto),
      bison::key_t{0U},
      dynamic::make_factory<tracked_stub_form>("wish"_key, "__TrackedStubForm"_key));
}

// ── Construction ──────────────────────────────────────────────────────────────

TEST(FormBase, ConstructFromEmptyDynamicDoesNotThrow) {
  EXPECT_NO_THROW({ stub_form f{dynamic{}}; });
}

TEST(FormBase, ConstructFromRegisteredClassDoesNotThrow) {
  ensure_registered();
  EXPECT_NO_THROW({
    auto f = dynamic::instantiate<stub_form>("wish"_key, "__StubForm"_key);
    (void)f;
  });
}

// ── init() ────────────────────────────────────────────────────────────────────

TEST(FormBase, InitCallsOnInitExactlyOnce) {
  stub_form f{dynamic{}};
  EXPECT_FALSE(f.init_called_);

  rmi::context ctx;
  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_init"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f.init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  EXPECT_TRUE(f.init_called_);
}

TEST(FormBase, InitStoresContextReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_ctx"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f.init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  EXPECT_EQ(f.ctx_ref_, &ctx);
}

TEST(FormBase, InitStoresSessionReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_sess"_key));

  wish::context* raw_sess;
  {
    auto lk = wish::context_wlock{*sync_sess};
    raw_sess = &(*lk);
    wish::detail::current_context = raw_sess;
    f.init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  EXPECT_EQ(f.sess_ref_, raw_sess);
}

// ── emit() ────────────────────────────────────────────────────────────────────
//
// form::emit() defers delivery via enqueue_event()/pending_events instead of
// invoking session::emit_event directly -- the same contract every widget
// event producer follows (see session.hpp's doc comment on emit_event). It
// never calls emit_event itself; the render loop is what drains
// pending_events and invokes it, after releasing the session lock.

TEST(FormBase, EmitForwardsEventToSession) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  // Place the form in ctx.objects so emit() can discover its ID.
  bison::key_t form_id{"stub_form_id"_key};
  ctx.objects[form_id.id] = f;

  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_emit"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }
  f->test_emit("on_test"_key);

  auto lk = wish::context_rlock{*sync_sess};
  ASSERT_EQ(lk->pending_events.size(), 1u);
  EXPECT_EQ(lk->pending_events[0].id.id, form_id.id);
  EXPECT_EQ(lk->pending_events[0].event_name.id, "on_test"_key.id);
}

TEST(FormBase, EmitWithPayloadForwardsPayload) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  bison::key_t form_id{"stub_payload_id"_key};
  ctx.objects[form_id.id] = f;

  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_emit_payload"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  dynamic payload;
  payload["path"_key] = std::string{"foo.txt"};
  f->test_emit("on_open"_key, std::move(payload));

  auto lk = wish::context_rlock{*sync_sess};
  ASSERT_EQ(lk->pending_events.size(), 1u);
  EXPECT_EQ(lk->pending_events[0].payload.as<std::string>("path"_key), "foo.txt");
}

TEST(FormBase, EmitBeforeInitDoesNothing) {
  stub_form f{dynamic{}};
  EXPECT_NO_THROW(f.test_emit("on_test"_key));
}

TEST(FormBase, EmitWithNullEmitEventStillEnqueues) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  bison::key_t form_id{"null_emit_id"_key};
  ctx.objects[form_id.id] = f;

  // Session with no emit_event callback set.
  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_null_emit"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  EXPECT_NO_THROW(f->test_emit("on_test"_key));

  // emit() always enqueues regardless of whether emit_event is set -- the
  // null-check happens later, at drain time (see the render loop).
  auto lk = wish::context_rlock{*sync_sess};
  EXPECT_EQ(lk->pending_events.size(), 1u);
}

// Regression: form::emit() must never invoke emit_event directly. In
// standalone mode emit_event invokes the registered onEvent handler
// synchronously and in-process (unlike server mode, where it just enqueues a
// network send), so a handler that calls back into another RMI operation
// would deadlock against whatever lock emit() held while calling it inline.
// This reproduced as a real hang in `wish standalone --run=nano` (clicking
// "Open" deadlocked). Verified here by confirming emit_event is simply never
// called by emit() -- it only shows up afterward in pending_events.
TEST(FormBase, EmitOutsideDispatchNeverInvokesEmitEventDirectly) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  bison::key_t form_id{"reentrant_emit_id"_key};
  ctx.objects[form_id.id] = f;

  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_reentrant_emit"_key));
  bool emit_event_called = false;
  {
    auto lk = wish::context_wlock{*sync_sess};
    lk->emit_event = [&emit_event_called](bison::key_t, bison::key_t, dynamic) { emit_event_called = true; };
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  f->test_emit("on_test"_key);

  EXPECT_FALSE(emit_event_called);

  auto lk = wish::context_rlock{*sync_sess};
  ASSERT_EQ(lk->pending_events.size(), 1u);
  EXPECT_EQ(lk->pending_events[0].id.id, form_id.id);
  EXPECT_EQ(lk->pending_events[0].event_name.id, "on_test"_key.id);
}

// Documents the full two-step contract: emit() enqueues (tested above), and
// draining pending_events -- exactly as server::render_loop() /
// standalone::render_loop() do -- is what actually invokes emit_event.
TEST(FormBase, DrainingPendingEventsInvokesEmitEventWithCorrectData) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  bison::key_t form_id{"drain_emit_id"_key};
  ctx.objects[form_id.id] = f;

  bison::key_t captured_id{};
  bison::key_t captured_event{};
  dynamic captured_payload;

  auto sync_sess = std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>("form_drain_emit"_key));
  {
    auto lk = wish::context_wlock{*sync_sess};
    lk->emit_event = [&](bison::key_t oid, bison::key_t evt, dynamic p) {
      captured_id = oid;
      captured_event = evt;
      captured_payload = std::move(p);
    };
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_context = nullptr;
  }

  dynamic payload;
  payload["path"_key] = std::string{"foo.txt"};
  f->test_emit("on_open"_key, std::move(payload));

  // Mirror the render loop: move pending_events out under the wlock, then
  // deliver them with no lock held.
  std::vector<wish::context::pending_event> events;
  std::function<void(bison::key_t, bison::key_t, dynamic)> emit_event;
  {
    auto lk = wish::context_wlock{*sync_sess};
    events = std::move(lk->pending_events);
    emit_event = lk->emit_event;
  }
  for (auto& ev : events)
    if (emit_event)
      emit_event(ev.id, ev.event_name, ev.payload);

  EXPECT_EQ(captured_id.id, form_id.id);
  EXPECT_EQ(captured_event.id, "on_open"_key.id);
  EXPECT_EQ(captured_payload.as<std::string>("path"_key), "foo.txt");
}

// ── Server injection ──────────────────────────────────────────────────────────

TEST(FormServerInjection, InstantiateFormCallsOnInit) {
  using namespace bdg::bison::rmi::transport;

  ensure_registered();
  g_server_init_count.store(0);

  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    bdg::bison::rmi::client c{transport.connect()};
    c.connect();
    auto proxy = c.instantiate("wish"_key, "__TrackedStubForm"_key).get();
    EXPECT_TRUE(proxy.valid());
    EXPECT_EQ(g_server_init_count.load(), 1);
    c.disconnect();
  }

  srv.stop();
}

TEST(FormServerInjection, TwoInstantiationsBothGetInit) {
  using namespace bdg::bison::rmi::transport;

  ensure_registered();
  g_server_init_count.store(0);

  memory_server_transport transport;
  wish::server srv{transport, std::make_unique<wish::null_renderer>()};
  srv.start();

  {
    bdg::bison::rmi::client c{transport.connect()};
    c.connect();
    auto p1 = c.instantiate("wish"_key, "__TrackedStubForm"_key).get();
    auto p2 = c.instantiate("wish"_key, "__TrackedStubForm"_key).get();
    EXPECT_TRUE(p1.valid());
    EXPECT_TRUE(p2.valid());
    EXPECT_EQ(g_server_init_count.load(), 2);
    c.disconnect();
  }

  srv.stop();
}

// ── set_default_dock_layout ──────────────────────────────────────────────────

namespace {

// A form that registers one window plus a default dock layout in on_init().
class dock_layout_form : public wish::form {
 public:
  explicit dock_layout_form(dynamic&& base) : form(std::move(base)) {}

 protected:
  void on_init() override {
    using namespace bdg::wish::dock;
    internal_root_key_ = next_available_key("__dlf_");
    auto win = bdg::wish::ui_element_ptr::create("wish"_key, "Window"_key);
    (*win)["title"_key] = std::string{"W"};
    sess().ui_objects[internal_root_key_] = win;

    set_default_dock_layout(layout(area({internal_root_key_}, internal_root_key_)));
  }
};

wish::sync_context_ptr make_dlf_session(const char* name) {
  return std::make_shared<wish::sync_context>(std::in_place, std::make_unique<wish::context>(bison::key_t{name}));
}

} // namespace

TEST(FormDockLayout, RegistersDockLayoutTopLevelObject) {
  ensure_registered();
  dock_layout_form f{dynamic{}};
  rmi::context ctx;
  auto sync_sess = make_dlf_session("form_dl_reg");
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f.init(ctx, sync_sess);

    auto it = lk->top_level_objects.find("__docklayout_0"_key);
    ASSERT_NE(it, lk->top_level_objects.end());
    EXPECT_EQ(it->second->class_key(), "DockLayout"_key);
    EXPECT_EQ(it->second->path_ref(), "__docklayout_0");

    // Its DockArea child names the window path.
    const bdg::wish::ui_element* area = nullptr;
    it->second->for_each_child_ordered([&](bison::key_t, bdg::wish::ui_element& c) { area = &c; });
    ASSERT_NE(area, nullptr);
    EXPECT_EQ(area->class_key(), "DockArea"_key);
    EXPECT_EQ(area->findField("windows"_key)->as<std::string>(), "__dlf_0");

    wish::detail::current_context = nullptr;
  }
}

TEST(FormDockLayout, TornDownWithForm) {
  ensure_registered();
  rmi::context ctx;
  auto sync_sess = make_dlf_session("form_dl_teardown");

  auto f = std::make_unique<dock_layout_form>(dynamic{});
  {
    auto lk = wish::context_wlock{*sync_sess};
    wish::detail::current_context = &(*lk);
    f->init(ctx, sync_sess);
    ASSERT_TRUE(lk->top_level_objects.count("__docklayout_0"_key));
    wish::detail::current_context = nullptr;
  }

  // ~form() runs outside dispatch and takes the session lock itself.
  f.reset();

  auto lk = wish::context_wlock{*sync_sess};
  EXPECT_FALSE(lk->top_level_objects.count("__docklayout_0"_key));
  EXPECT_FALSE(lk->ui_objects.count("__docklayout_0"));
}
