// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <wish/form.hpp>
#include <wish/registry.hpp>
#include <wish/session.hpp>

#include "src/bison/bison_object.hpp"
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
  auto sess = std::make_shared<wish::session>("form_init"_key);
  f.init(ctx, sess);

  EXPECT_TRUE(f.init_called_);
}

TEST(FormBase, InitStoresContextReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sess = std::make_shared<wish::session>("form_ctx"_key);
  f.init(ctx, sess);

  EXPECT_EQ(f.ctx_ref_, &ctx);
}

TEST(FormBase, InitStoresSessionReference) {
  stub_form f{dynamic{}};
  rmi::context ctx;
  auto sess = std::make_shared<wish::session>("form_sess"_key);
  f.init(ctx, sess);

  EXPECT_EQ(f.sess_ref_, sess.get());
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

  auto sess = std::make_shared<wish::session>("form_emit"_key);
  sess->emit_event = [&](key_t oid, key_t evt, dynamic) {
    captured_id    = oid;
    captured_event = evt;
  };

  f->init(ctx, sess);
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

  auto sess = std::make_shared<wish::session>("form_emit_payload"_key);
  sess->emit_event = [&](key_t, key_t, dynamic p) {
    captured_payload = std::move(p);
  };

  f->init(ctx, sess);

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
  auto sess = std::make_shared<wish::session>("form_null_emit"_key);
  f->init(ctx, sess);

  EXPECT_NO_THROW(f->test_emit("on_test"_key));
}
