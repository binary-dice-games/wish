// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/form.hpp>
#include <wish/registry.hpp>
#include <wish/server.hpp>
#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/server/context.hpp"

using namespace bdg::bison;
namespace wish = bdg::wish;
namespace rmi  = bdg::bison::rmi;

// ── stub_form ─────────────────────────────────────────────────────────────────

/// Minimal concrete form used throughout these tests.
class stub_form : public wish::form {
 public:
  explicit stub_form(dynamic&& base) : form(std::move(base)) {}

  bool          init_called_{false};
  rmi::context* ctx_ref_{nullptr};
  wish::session* sess_ref_{nullptr};

  /// Expose emit() for testing (it is protected in form).
  void test_emit(key_t event_name, dynamic payload = {}) {
    emit(event_name, std::move(payload));
  }

 protected:
  void on_init() override {
    init_called_ = true;
    ctx_ref_  = &ctx();
    sess_ref_ = &sess();
  }
};

// ── Registration helper ───────────────────────────────────────────────────────

static void ensure_registered() {
  // register_all() is idempotent; registering stub_form only needs to happen
  // once per process.
  static bool done = false;
  if (done) return;
  done = true;
  bdg::wish::register_all();
  auto proto = dynamic_ptr{"__StubForm"_key, {}};
  dynamic::addClass(
      "wish"_key,
      std::move(proto),
      key_t{0U},
      dynamic::make_factory<stub_form>("wish"_key, "__StubForm"_key));
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
  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_init"_key});
  {
    auto lk = sync_sess->wlock();
    wish::detail::current_session = &(*lk);
    f.init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }

  EXPECT_TRUE(f.init_called_);
}

TEST(FormBase, InitStoresContextReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_ctx"_key});
  {
    auto lk = sync_sess->wlock();
    wish::detail::current_session = &(*lk);
    f.init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }

  EXPECT_EQ(f.ctx_ref_, &ctx);
}

TEST(FormBase, InitStoresSessionReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_sess"_key});

  wish::session* raw_sess;
  {
    auto lk = sync_sess->wlock();
    raw_sess = &(*lk);
    wish::detail::current_session = raw_sess;
    f.init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }

  EXPECT_EQ(f.sess_ref_, raw_sess);
}

// ── emit() ────────────────────────────────────────────────────────────────────

TEST(FormBase, EmitForwardsEventToSession) {
  auto f = std::make_shared<stub_form>(dynamic{});

  key_t captured_id{};
  key_t captured_event{};

  rmi::context ctx;
  // Place the form in ctx.objects so emit() can discover its ID.
  key_t form_id{"stub_form_id"_key};
  ctx.objects[form_id.id] = f;

  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_emit"_key});
  {
    auto lk = sync_sess->wlock();
    lk->emit_event = [&](key_t oid, key_t evt, dynamic) {
      captured_id    = oid;
      captured_event = evt;
    };
    wish::detail::current_session = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }
  f->test_emit("on_test"_key);

  EXPECT_EQ(captured_id.id,    form_id.id);
  EXPECT_EQ(captured_event.id, "on_test"_key.id);
}

TEST(FormBase, EmitWithPayloadForwardsPayload) {
  auto f = std::make_shared<stub_form>(dynamic{});

  dynamic captured_payload;

  rmi::context ctx;
  key_t form_id{"stub_payload_id"_key};
  ctx.objects[form_id.id] = f;

  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_emit_payload"_key});
  {
    auto lk = sync_sess->wlock();
    lk->emit_event = [&](key_t, key_t, dynamic p) {
      captured_payload = std::move(p);
    };
    wish::detail::current_session = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }

  dynamic payload;
  payload["path"_key] = std::string{"foo.txt"};
  f->test_emit("on_open"_key, std::move(payload));

  EXPECT_EQ(captured_payload.as<std::string>("path"_key), "foo.txt");
}

TEST(FormBase, EmitBeforeInitDoesNothing) {
  stub_form f{dynamic{}};
  EXPECT_NO_THROW(f.test_emit("on_test"_key));
}

TEST(FormBase, EmitWithNullEmitEventDoesNothing) {
  auto f = std::make_shared<stub_form>(dynamic{});

  rmi::context ctx;
  key_t form_id{"null_emit_id"_key};
  ctx.objects[form_id.id] = f;

  // Session with no emit_event callback set.
  auto sync_sess = std::make_shared<wish::sync_session>(wish::session{"form_null_emit"_key});
  {
    auto lk = sync_sess->wlock();
    wish::detail::current_session = &(*lk);
    f->init(ctx, sync_sess);
    wish::detail::current_session = nullptr;
  }

  EXPECT_NO_THROW(f->test_emit("on_test"_key));
}

// ── Server injection ──────────────────────────────────────────────────────────

// Track which stub_form instances had init() called.
static std::atomic<int> g_server_init_count{0};

class tracked_stub_form : public wish::form {
 public:
  explicit tracked_stub_form(dynamic&& base) : form(std::move(base)) {}
 protected:
  void on_init() override { g_server_init_count.fetch_add(1); }
};

TEST(FormServerInjection, InstantiateFormCallsOnInit) {
  using namespace bdg::bison::rmi::transport;

  ensure_registered();
  // Register tracked_stub_form as a separate class.
  static bool tracked_registered = false;
  if (!tracked_registered) {
    tracked_registered = true;
    auto proto = dynamic_ptr{"__TrackedStubForm"_key, {}};
    dynamic::addClass(
        "wish"_key,
        std::move(proto),
        key_t{0U},
        dynamic::make_factory<tracked_stub_form>("wish"_key, "__TrackedStubForm"_key));
  }

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
