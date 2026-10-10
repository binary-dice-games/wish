// MIT License © 2026 Binary Dice Games
/// @file test_bc.cpp
/// @brief Tests for bc: the calc::engine arithmetic (standard, scientific and
///        programmer modes) and the form's keypads, which drive the display
///        through common::tool_form's click dispatch.
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "modules/bdg/desktop/bc/server/calc_engine.hpp"
#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <memory>
#include <optional>
#include <string>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
namespace calc = bdg::wish::calc;
using namespace bdg::bison::rmi::transport;

// ── Engine: entry and standard mode ──────────────────────────────────────────

namespace {

// Types a decimal number, e.g. "12.5".
void type(calc::engine& e, const std::string& digits) {
  for (char c : digits) {
    if (c == '.')
      e.point();
    else
      e.digit(c >= 'A' ? c - 'A' + 10 : c - '0');
  }
}

const std::string kTimes = "\xC3\x97";

} // namespace

TEST(BcEngine, StandardModeEvaluatesLeftToRight) {
  calc::engine e;
  type(e, "2");
  e.binary(calc::binary_op::add);
  type(e, "3");
  e.binary(calc::binary_op::multiply);
  EXPECT_EQ(e.display(), "5"); // immediate execution shows the running total
  type(e, "4");
  e.equals();
  EXPECT_EQ(e.display(), "20");
  EXPECT_EQ(e.expression(), "2 + 3 " + kTimes + " 4 =");
  ASSERT_EQ(e.history().size(), 1u);
  EXPECT_EQ(e.history()[0].result, "20");
}

TEST(BcEngine, EntryEditingAndGrouping) {
  calc::engine e;
  type(e, "1234567.50");
  EXPECT_EQ(e.display(), "1,234,567.50");
  e.backspace();
  e.backspace();
  EXPECT_EQ(e.display(), "1,234,567.");
  e.negate();
  EXPECT_EQ(e.display(), "-1,234,567.");
  e.set_digit_grouping(false);
  EXPECT_EQ(e.display(), "-1234567.");
  e.clear_entry();
  EXPECT_EQ(e.display(), "0");
}

TEST(BcEngine, RepeatedEqualsRepeatsTheLastOperation) {
  calc::engine e;
  type(e, "5");
  e.binary(calc::binary_op::add);
  type(e, "3");
  e.equals();
  e.equals();
  EXPECT_EQ(e.display(), "11");
  EXPECT_EQ(e.expression(), "8 + 3 =");
}

TEST(BcEngine, PercentAndUnaryFunctions) {
  calc::engine e;
  type(e, "50");
  e.binary(calc::binary_op::add);
  type(e, "10");
  e.percent();
  EXPECT_EQ(e.display(), "5");
  e.equals();
  EXPECT_EQ(e.display(), "55");

  e.clear();
  type(e, "9");
  e.unary(calc::unary_op::sqrt);
  e.unary(calc::unary_op::reciprocal);
  EXPECT_EQ(e.display(), "0.333333333333333");
  EXPECT_EQ(e.expression(), "1/(\xE2\x88\x9A(9))");
}

TEST(BcEngine, ErrorsAreReportedAndClearedByNewInput) {
  calc::engine e;
  type(e, "1");
  e.binary(calc::binary_op::divide);
  type(e, "0");
  e.equals();
  EXPECT_TRUE(e.has_error());
  EXPECT_EQ(e.display(), "Cannot divide by zero");
  e.binary(calc::binary_op::add); // ignored while in error
  EXPECT_TRUE(e.has_error());
  type(e, "4");
  EXPECT_FALSE(e.has_error());
  EXPECT_EQ(e.display(), "4");

  e.negate();
  e.unary(calc::unary_op::sqrt);
  EXPECT_EQ(e.display(), "Invalid input");
}

TEST(BcEngine, MemoryKeys) {
  calc::engine e;
  EXPECT_FALSE(e.has_memory());
  type(e, "7");
  e.memory_store();
  type(e, "3");
  e.memory_add();
  e.clear();
  e.memory_recall();
  EXPECT_EQ(e.display(), "10");
  type(e, "4");
  e.memory_subtract();
  e.memory_recall();
  EXPECT_EQ(e.display(), "6");
  e.memory_clear();
  EXPECT_FALSE(e.has_memory());
}

TEST(BcEngine, HistoryRecall) {
  calc::engine e;
  type(e, "6");
  e.binary(calc::binary_op::multiply);
  type(e, "7");
  e.equals();
  e.clear();
  e.recall_history(0);
  EXPECT_EQ(e.display(), "42");
  e.clear_history();
  EXPECT_TRUE(e.history().empty());
}

// ── Engine: scientific mode ───────────────────────────────────────────────────

TEST(BcEngine, ScientificModeHonorsPrecedence) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  type(e, "2");
  e.binary(calc::binary_op::add);
  type(e, "3");
  e.binary(calc::binary_op::multiply);
  type(e, "4");
  e.binary(calc::binary_op::power);
  type(e, "2");
  e.equals();
  EXPECT_EQ(e.display(), "50"); // 2 + 3 * 4^2
}

TEST(BcEngine, ParenthesesAndImplicitMultiplication) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  type(e, "2");
  e.open_paren(); // implies 2 * (
  type(e, "1");
  e.binary(calc::binary_op::add);
  type(e, "2");
  EXPECT_EQ(e.open_parens(), 1);
  e.close_paren();
  EXPECT_EQ(e.display(), "3");
  e.unary(calc::unary_op::square);
  EXPECT_EQ(e.expression(), "2 " + kTimes + " sqr(1 + 2)");
  e.equals();
  EXPECT_EQ(e.display(), "18");
  EXPECT_EQ(e.open_parens(), 0);
}

TEST(BcEngine, EqualsClosesOpenParentheses) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  e.open_paren();
  type(e, "4");
  e.binary(calc::binary_op::subtract);
  type(e, "1");
  e.equals();
  EXPECT_EQ(e.display(), "3");
  EXPECT_EQ(e.expression(), "(4 - 1) =");
}

TEST(BcEngine, TrigonometryInEachAngleUnit) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  type(e, "30");
  e.unary(calc::unary_op::sin);
  EXPECT_EQ(e.display(), "0.5");
  type(e, "180");
  e.unary(calc::unary_op::sin);
  EXPECT_EQ(e.display(), "0"); // exact, not 1.2e-16
  type(e, "90");
  e.unary(calc::unary_op::tan);
  EXPECT_TRUE(e.has_error());

  e.clear();
  e.set_angle_unit(calc::angle_unit::gradians);
  type(e, "100");
  e.unary(calc::unary_op::sin);
  EXPECT_EQ(e.display(), "1");

  e.set_angle_unit(calc::angle_unit::radians);
  e.constant(3.14159265358979323846, "pi");
  e.unary(calc::unary_op::cos);
  EXPECT_EQ(e.display(), "-1");

  e.set_angle_unit(calc::angle_unit::degrees);
  type(e, "1");
  e.unary(calc::unary_op::atan);
  EXPECT_EQ(e.display(), "45");
}

TEST(BcEngine, ScientificFunctions) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  auto apply = [&](const std::string& x, calc::unary_op op) {
    e.clear();
    type(e, x);
    e.unary(op);
    return e.display();
  };
  EXPECT_EQ(apply("5", calc::unary_op::factorial), "120");
  EXPECT_EQ(apply("1000", calc::unary_op::log10), "3");
  EXPECT_EQ(apply("1024", calc::unary_op::log2), "10");
  EXPECT_EQ(apply("27", calc::unary_op::cbrt), "3");
  EXPECT_EQ(apply("3", calc::unary_op::pow10), "1,000");
  EXPECT_EQ(apply("2.5", calc::unary_op::floor), "2");
  EXPECT_EQ(apply("2.5", calc::unary_op::ceil), "3");
  EXPECT_EQ(apply("0", calc::unary_op::exp), "1");

  e.clear();
  type(e, "8");
  e.negate();
  e.binary(calc::binary_op::root);
  type(e, "3");
  e.equals();
  EXPECT_EQ(e.display(), "-2");

  e.clear();
  type(e, "81");
  e.binary(calc::binary_op::log_base);
  type(e, "3");
  e.equals();
  EXPECT_EQ(e.display(), "4");

  e.clear();
  type(e, "17");
  e.binary(calc::binary_op::modulo);
  type(e, "5");
  e.equals();
  EXPECT_EQ(e.display(), "2");
}

TEST(BcEngine, ExponentEntryAndScientificNotation) {
  calc::engine e;
  e.set_mode(calc::mode::scientific);
  type(e, "1.5");
  e.exponent();
  type(e, "3");
  EXPECT_EQ(e.display(), "1.5e+3");
  e.negate(); // flips the exponent's sign while typing it
  EXPECT_EQ(e.display(), "1.5e-3");
  e.equals();
  EXPECT_EQ(e.display(), "0.0015");
  e.set_scientific_notation(true);
  EXPECT_EQ(e.display(), "1.5e-3");

  EXPECT_EQ(calc::engine::format_real(1e20), "1e+20");
  EXPECT_EQ(calc::engine::format_real(0.1 + 0.2), "0.3");
  EXPECT_EQ(calc::engine::format_real(-0.0), "0");
}

// ── Engine: programmer mode ───────────────────────────────────────────────────

TEST(BcEngine, ProgrammerModeShowsEveryRadix) {
  calc::engine e;
  e.set_mode(calc::mode::programmer);
  type(e, "255");
  EXPECT_EQ(e.display_in(16), "FF");
  EXPECT_EQ(e.display_in(8), "377");
  EXPECT_EQ(e.display_in(2), "1111 1111");
  EXPECT_EQ(e.display_in(10), "255");

  e.set_radix(16);
  e.clear();
  type(e, "1F");
  EXPECT_EQ(e.display_in(10), "31");
  e.digit(15);
  EXPECT_EQ(e.display(), "1FF");

  e.set_radix(2);
  e.clear();
  e.digit(2); // not a binary digit
  EXPECT_EQ(e.display(), "0");
}

TEST(BcEngine, ProgrammerWordSizeWrapsAround) {
  calc::engine e;
  e.set_mode(calc::mode::programmer);
  e.set_word_size(calc::word_size::byte);
  type(e, "127");
  e.binary(calc::binary_op::add);
  type(e, "1");
  e.equals();
  EXPECT_EQ(e.display(), "-128");
  EXPECT_EQ(e.display_in(16), "80");

  e.set_radix(16);
  e.clear();
  type(e, "FF1"); // the third digit would overflow a byte
  EXPECT_EQ(e.display(), "FF");
  EXPECT_EQ(e.display_in(10), "-1");

  e.set_word_size(calc::word_size::qword);
  e.clear();
  type(e, "FFFF");
  e.set_word_size(calc::word_size::byte); // truncates the current value
  EXPECT_EQ(e.display(), "FF");
}

TEST(BcEngine, ProgrammerBitwiseAndShifts) {
  calc::engine e;
  e.set_mode(calc::mode::programmer);
  auto eval = [&](const std::string& a, calc::binary_op op, const std::string& b) {
    e.clear();
    type(e, a);
    e.binary(op);
    type(e, b);
    e.equals();
    return e.display();
  };
  EXPECT_EQ(eval("12", calc::binary_op::bit_and, "10"), "8");
  EXPECT_EQ(eval("12", calc::binary_op::bit_or, "3"), "15");
  EXPECT_EQ(eval("12", calc::binary_op::bit_xor, "10"), "6");
  EXPECT_EQ(eval("1", calc::binary_op::shift_left, "10"), "1,024");
  EXPECT_EQ(eval("7", calc::binary_op::divide, "2"), "3");
  EXPECT_EQ(eval("7", calc::binary_op::modulo, "4"), "3");

  // Precedence: shifts bind tighter than OR.
  e.clear();
  type(e, "1");
  e.binary(calc::binary_op::shift_left);
  type(e, "4");
  e.binary(calc::binary_op::bit_or);
  type(e, "1");
  e.equals();
  EXPECT_EQ(e.display(), "17");

  e.set_word_size(calc::word_size::byte);
  e.clear();
  type(e, "0");
  e.unary(calc::unary_op::bit_not);
  EXPECT_EQ(e.display(), "-1");
  e.clear();
  type(e, "129"); // DEC entry stops at a byte's signed maximum
  EXPECT_EQ(e.display(), "12");
  e.set_radix(16);
  e.clear();
  type(e, "81"); // 1000 0001 -> RoL -> 0000 0011
  e.unary(calc::unary_op::rotate_left);
  EXPECT_EQ(e.display(), "3");
  e.unary(calc::unary_op::rotate_right);
  e.unary(calc::unary_op::rotate_right);
  EXPECT_EQ(e.display(), "C0");
}

TEST(BcEngine, ModeSwitchKeepsTheDisplayedValue) {
  calc::engine e;
  type(e, "42.9");
  e.set_mode(calc::mode::programmer);
  EXPECT_EQ(e.display(), "42");
  e.set_mode(calc::mode::scientific);
  EXPECT_EQ(e.display(), "42");
}

// ── Form: keypads through click dispatch ─────────────────────────────────────

namespace {

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
  }
};

} // namespace

class BcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "Bc"_key).get());
    ASSERT_TRUE(proxy_->valid());
    for (const auto& [k, _] : srv_->last_session->ui_objects)
      if (k.rfind("__calc_", 0) == 0 && k.find('.') == std::string::npos)
        root_ = k;
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  // The element at @p path below the form's root; fails the test if absent.
  auto element(const std::string& path) {
    auto it = srv_->last_session->ui_objects.find(root_ + "." + path);
    EXPECT_NE(it, srv_->last_session->ui_objects.end()) << path;
    return it == srv_->last_session->ui_objects.end() ? decltype(it->second){} : it->second;
  }

  // Sends @p event from the widget at @p path (e.g. "body.main.std.r2.n7").
  void fire(const std::string& path, bison::key_t event = "clicked"_key) {
    auto el = element(path);
    ASSERT_TRUE(el) << path;
    auto h = srv_->last_session->top_level_handlers.find(bison::key_t{root_});
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(el->as<bison::key_t>("__wish_id"_key), event, dynamic{});
  }

  // Presses key @p id of the visible keypad @p pad ("std", "sci", "prog").
  void press(const std::string& pad, const std::string& id) {
    for (int r = 0; r < 6; ++r) {
      auto it = srv_->last_session->ui_objects.find(root_ + ".body.main." + pad + ".r" + std::to_string(r) + "." + id);
      if (it != srv_->last_session->ui_objects.end()) {
        fire("body.main." + pad + ".r" + std::to_string(r) + "." + id);
        return;
      }
    }
    ADD_FAILURE() << "no key " << id << " on keypad " << pad;
  }

  std::string text(const std::string& path, bison::key_t field = "text"_key) {
    return element(path)->as<std::string>(field);
  }
  bool visible(const std::string& path) {
    return element(path)->as<bool>("visible"_key);
  }
  std::string display() {
    return text("body.main.display_row.display");
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bison::rmi::client> client_;
  std::optional<bison::rmi::proxy::dynamic> proxy_;
  std::string root_;
};

TEST_F(BcTest, StandardKeypadComputesASum) {
  EXPECT_EQ(display(), "0");
  EXPECT_TRUE(visible("body.main.std"));
  EXPECT_FALSE(visible("body.main.sci"));
  press("std", "n7");
  press("std", "add");
  press("std", "n1");
  EXPECT_EQ(display(), "1");
  press("std", "eq");
  EXPECT_EQ(display(), "8");
  EXPECT_EQ(text("body.main.expr_row.expr"), "7 + 1 =");
}

TEST_F(BcTest, ClearResetsTheDisplay) {
  press("std", "n9");
  press("std", "n8");
  EXPECT_EQ(display(), "98");
  press("std", "c");
  EXPECT_EQ(display(), "0");
}

TEST_F(BcTest, ModeTabsSwitchTheKeypad) {
  fire("body.main.tabs.sci", "changed"_key);
  EXPECT_TRUE(visible("body.main.sci"));
  EXPECT_FALSE(visible("body.main.std"));
  EXPECT_TRUE(element("body.main.tabs.sci")->as<bool>("selected"_key));
  EXPECT_TRUE(element("menu.view.sci")->as<bool>("checked"_key));

  fire("menu.view.prog");
  EXPECT_TRUE(visible("body.main.prog"));
  EXPECT_TRUE(visible("body.main.radix"));
  EXPECT_FALSE(visible("body.main.sci"));
}

TEST_F(BcTest, ScientificSecondFunctionsRelabelAndApply) {
  fire("body.main.tabs.sci", "changed"_key);
  press("sci", "n2");
  press("sci", "add");
  press("sci", "n3");
  press("sci", "mul");
  press("sci", "n4");
  press("sci", "eq");
  EXPECT_EQ(display(), "14");

  press("sci", "c");
  press("sci", "n8");
  press("sci", "2nd");
  EXPECT_EQ(text("body.main.sci.r2.sq", "label"_key), "x\xC2\xB3");
  press("sci", "sq"); // x^3 while 2nd is on, then 2nd releases
  EXPECT_EQ(display(), "512");
  EXPECT_EQ(text("body.main.sci.r2.sq", "label"_key), "x\xC2\xB2");

  press("sci", "c");
  press("sci", "n1");
  press("sci", "2nd");
  press("sci", "tan");
  EXPECT_EQ(display(), "45"); // atan(1) in degrees
}

TEST_F(BcTest, ProgrammerRadixRowsFollowTheValue) {
  fire("body.main.tabs.prog", "changed"_key);
  press("prog", "n2");
  press("prog", "n5");
  press("prog", "n5");
  EXPECT_EQ(text("body.main.radix.hex", "label"_key), "HEX   FF");
  EXPECT_EQ(text("body.main.radix.bin", "label"_key), "BIN   1111 1111");

  fire("body.main.radix.hex", "changed"_key);
  EXPECT_TRUE(element("body.main.radix.hex")->as<bool>("selected"_key));
  EXPECT_FALSE(element("body.main.radix.dec")->as<bool>("selected"_key));
  press("prog", "c");
  press("prog", "nA");
  EXPECT_EQ(display(), "A");
  EXPECT_EQ(text("body.main.radix.dec", "label"_key), "DEC   10");
}

TEST_F(BcTest, HistoryPanelListsAndRecallsResults) {
  EXPECT_FALSE(visible("body.history"));
  fire("menu.view.history");
  EXPECT_TRUE(visible("body.history"));
  EXPECT_TRUE(visible("body.history.empty"));

  press("std", "n6");
  press("std", "mul");
  press("std", "n7");
  press("std", "eq");
  EXPECT_FALSE(visible("body.history.empty"));
  EXPECT_TRUE(visible("body.history.h0"));
  EXPECT_FALSE(visible("body.history.h1"));
  EXPECT_EQ(text("body.history.h0", "label"_key), "6 " + kTimes + " 7 =\n42");

  press("std", "c");
  fire("body.history.h0", "changed"_key);
  EXPECT_EQ(display(), "42");
}

TEST_F(BcTest, MemoryKeysAndCopyText) {
  press("std", "n5");
  fire("body.main.mem.ms");
  EXPECT_EQ(text("body.main.status"), "M");
  press("std", "c");
  fire("body.main.mem.mr");
  EXPECT_EQ(display(), "5");
  EXPECT_EQ(text("menu.edit.copy", "copy_text"_key), "5");
}
