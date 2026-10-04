// MIT License © 2026 Binary Dice Games
/// @file test_dev_common.cpp
/// @brief Tests for the helpers shared by the bdg/dev modules' clients
///        (modules/bdg/common): run_process(), the text helpers, and
///        tool_source's Console-trace / command_result plumbing.
#include <gtest/gtest.h>

#include "modules/bdg/common/process.hpp"
#include "modules/bdg/common/text.hpp"
#include "modules/bdg/common/tool_source.hpp"

#include <context/context.hpp>
#include <server/registry.hpp>
#include <server/server.hpp>
#include <ui/forms/form.hpp>

#include "src/client/wish_app_host.hpp"

#include "src/bison/bison_object.hpp"
#include "src/bison/bison_sync.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace common = bdg::wish::common;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// ── run_process() ───────────────────────────────────────────────────────────
//
// Exercised with stub programs (printf / false / cat / sh -- never a real
// dev tool) so these pass on any machine.

TEST(DevProcessTest, CapturesStdout) {
  auto r = common::run_process({"printf", "%s|%s", "a", "b c"});
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.stdout_text, "a|b c") << "an argument with a space stays one argv entry";
}

TEST(DevProcessTest, ShellMetacharactersNeedNoEscaping) {
  // No shell in the pipeline: braces, quotes and spaces pass through verbatim.
  const std::string arg = "{range .items[*]}{.metadata.name}{\"\\n\"}{end} 'x y' $HOME";
  auto r = common::run_process({"printf", "%s", arg});
  EXPECT_EQ(r.stdout_text, arg);
}

TEST(DevProcessTest, CapturesStderrAndNonZeroExitCode) {
  auto r = common::run_process({"sh", "-c", "echo oops >&2; exit 3"});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.exit_code, 3);
  EXPECT_EQ(r.stderr_text, "oops\n");
  EXPECT_EQ(common::error_output(r), "oops\n");
}

TEST(DevProcessTest, ErrorOutputFallsBackToStdout) {
  auto r = common::run_process({"sh", "-c", "echo said-on-stdout; exit 1"});
  EXPECT_EQ(common::error_output(r), "said-on-stdout\n");
}

TEST(DevProcessTest, MissingProgramIsExitCodeMinusOne) {
  auto r = common::run_process({"definitely-not-a-real-binary-xyzzy", "--version"});
  EXPECT_EQ(r.exit_code, -1);
  EXPECT_FALSE(r.stderr_text.empty());
}

TEST(DevProcessTest, EmptyArgvIsRejected) {
  EXPECT_EQ(common::run_process({}).exit_code, -1);
  EXPECT_EQ(common::run_process({""}).exit_code, -1);
}

TEST(DevProcessTest, AFailedSpawnDoesNotBreakTheNextOne) {
  // Modules probe for programs that are usually absent (pkexec, the other
  // package managers) and then carry on in the same process.
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(common::run_process({"definitely-not-a-real-binary-xyzzy"}).exit_code, -1);
  auto r = common::run_process({"printf", "still works"});
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "still works");
}

TEST(DevProcessTest, RunsInTheGivenWorkingDirectory) {
  common::process_options options;
  options.cwd = "/";
  auto r = common::run_process({"pwd"}, options);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(common::trim_eol(r.stdout_text), "/");
}

TEST(DevProcessTest, StdinTextIsDeliveredToTheChild) {
  common::process_options options;
  options.stdin_text = "s3cret\n";
  auto r = common::run_process({"cat"}, options);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "s3cret\n");
}

TEST(DevProcessTest, ChildThatIgnoresStdinDoesNotCrashUs) {
  // `true` exits without reading; the write must not raise SIGPIPE.
  common::process_options options;
  options.stdin_text = std::string(1 << 20, 'x');
  EXPECT_TRUE(common::run_process({"true"}, options).ok());
}

TEST(DevProcessTest, HooksDeliverOutputAsItArrives) {
  common::run_hooks hooks;
  std::string seen;
  hooks.on_output = [&](const std::string& chunk) { seen += chunk; };
  common::process_options options;
  options.hooks = &hooks;
  auto r = common::run_process({"printf", "%s", "hello"}, options);
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(seen, "hello");
  EXPECT_EQ(r.stdout_text, "hello") << "the result still carries the whole output";
}

TEST(DevProcessTest, TickReturningFalseStopsTheProcess) {
  common::run_hooks hooks;
  hooks.tick_ms = 20;
  int ticks = 0;
  hooks.on_tick = [&] { return ++ticks < 2; };
  common::process_options options;
  options.hooks = &hooks;
  auto r = common::run_process({"sleep", "60"}, options); // returns long before 60 s
  EXPECT_FALSE(r.ok());
  EXPECT_NE(r.exit_code, -1) << "it was spawned, then stopped";
  EXPECT_EQ(ticks, 2) << "no ticks after the stop request";
}

TEST(DevProcessTest, StoppedProcessReturnsEvenIfAChildKeepsThePipesOpen) {
  // The shell dies on SIGTERM; its background `sleep` inherits stdout /
  // stderr and outlives it (git's remote helper, a pip build step).
  common::run_hooks hooks;
  hooks.tick_ms = 20;
  hooks.on_tick = [] { return false; };
  common::process_options options;
  options.hooks = &hooks;
  const auto started = std::chrono::steady_clock::now();
  auto r = common::run_process({"sh", "-c", "sleep 30 & wait"}, options);
  EXPECT_FALSE(r.ok());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{10});
}

TEST(DevProcessTest, ConcatArgsPrependsALauncher) {
  EXPECT_EQ(
      common::concat_args({"python3", "-m", "pip"}, {"list"}),
      (std::vector<std::string>{"python3", "-m", "pip", "list"}));
  EXPECT_EQ(common::concat_args({}, {"a"}), (std::vector<std::string>{"a"}));
}

// ── text helpers ────────────────────────────────────────────────────────────

TEST(DevTextTest, TrimAndTrimEol) {
  EXPECT_EQ(common::trim(" \t a b \r\n"), "a b");
  EXPECT_EQ(common::trim(" \n"), "");
  EXPECT_EQ(common::trim_eol(" a \r\n\n"), " a ");
}

TEST(DevTextTest, SplitKeepsEmptyFields) {
  EXPECT_EQ(common::split("a\t\tb", '\t'), (std::vector<std::string>{"a", "", "b"}));
  EXPECT_EQ(common::split("a,", ','), (std::vector<std::string>{"a", ""}));
  EXPECT_EQ(common::split("", ','), (std::vector<std::string>{""}));
}

TEST(DevTextTest, WordsDropsWhitespaceRuns) {
  EXPECT_EQ(common::words("  numpy \t pandas==2.2\n"), (std::vector<std::string>{"numpy", "pandas==2.2"}));
  EXPECT_TRUE(common::words(" \n").empty());
}

TEST(DevTextTest, OneLineCollapsesWhitespaceAndCuts) {
  EXPECT_EQ(common::one_line("a\t\tb\r\n c \n", 100), "a b c");
  EXPECT_EQ(common::one_line("abcdef", 3), "abc...");
  EXPECT_EQ(common::one_line("", 3), "");
}

TEST(DevTextTest, FromMarkerLineDropsChatterBeforeTheFirstMarker) {
  EXPECT_EQ(common::from_marker_line("warn 1\nwarn 2\nError: boom\nmore\n", {"Error:"}), "Error: boom\nmore\n");
  EXPECT_EQ(common::from_marker_line("Error: first\nError: second", {"Error:"}), "Error: first\nError: second");
  EXPECT_EQ(common::from_marker_line("no marker here\n", {"Error:"}), "no marker here\n");
  // The earliest line matching any marker wins.
  EXPECT_EQ(common::from_marker_line("x\nerror: b\nE: a\n", {"E: ", "error: "}), "error: b\nE: a\n");
}

TEST(DevTextTest, FlagShapedOrEmptyValuesAreNotSafeArgs) {
  EXPECT_TRUE(common::is_safe_arg("web"));
  EXPECT_TRUE(common::is_safe_arg("bitnami/nginx"));
  EXPECT_FALSE(common::is_safe_arg(""));
  EXPECT_FALSE(common::is_safe_arg("-n"));
  EXPECT_FALSE(common::is_safe_arg("--post-renderer=/tmp/x"));
}

// ── tool_source ─────────────────────────────────────────────────────────────
//
// Driven against a stub server-side form that records the RMI calls a dev
// module's form receives (append_command_log, command_result, update_rows).

struct recorded_call {
  std::string method;
  dynamic args;
};

bison::synchronized<std::vector<recorded_call>>& recorded_calls() {
  static bison::synchronized<std::vector<recorded_call>> calls;
  return calls;
}

class recording_form : public wish::form {
 public:
  explicit recording_form(dynamic&& base) : form(std::move(base)) {}

 protected:
  void on_init() override {}
};

void ensure_registered() {
  static bool done = false;
  if (done)
    return;
  done = true;
  wish::register_all();
  auto proto = dynamic_ptr{"__ToolSourceStub"_key, {}};
  for (const char* name : {"append_command_log", "command_result", "update_rows"}) {
    const std::string method = name;
    proto->addMethod(bison::key_t{name}, bison::method{[method](dynamic&, const dynamic& args) -> dynamic {
                       recorded_calls().wlock()->push_back({method, args});
                       return {};
                     }});
  }
  dynamic::addClass(
      "wish"_key, std::move(proto), bison::key_t{0U},
      dynamic::make_factory<recording_form>("wish"_key, "__ToolSourceStub"_key));
}

/// wish_app_host over a plain RMI client. Only instantiate() is ever used
/// (by command_worker, for its progress dialog).
class client_app_host : public wish::wish_app_host {
 public:
  explicit client_app_host(bison::rmi::client& client) : client_(client) {}

  std::future<bison::rmi::proxy::dynamic> instantiate(bison::key_t ns, bison::key_t klass, dynamic params) override {
    return client_.instantiate(ns, klass, std::move(params));
  }
  std::future<void> upload_file(const std::string&, const std::string&, wish::transfer_progress_callback) override {
    return {};
  }
  std::future<std::string> download_file(const std::string&, wish::transfer_progress_callback) override {
    return {};
  }
  void keep_alive(bison::rmi::proxy::dynamic&&) override {}
  void signal_done() override {}
  const std::vector<std::string>& app_args() const override {
    return args_;
  }
  bool read_console_line(std::string&) override {
    return false;
  }

 private:
  bison::rmi::client& client_;
  std::vector<std::string> args_;
};

/// Exposes tool_source's protected API to the tests.
class test_source : public common::tool_source {
 public:
  test_source(
      std::shared_ptr<bison::rmi::proxy::dynamic> proxy, std::shared_ptr<common::command_worker> worker, std::string tool,
      std::vector<std::string> launcher)
      : tool_source(std::move(proxy), std::move(worker), std::move(tool), std::move(launcher)) {}

  using tool_source::call;
  using tool_source::caption;
  using tool_source::push_rows;
  using tool_source::report;
  using tool_source::run_logged;
};

class ToolSourceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ensure_registered();
    recorded_calls().wlock()->clear();
    srv_ = std::make_unique<wish::server>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_ = std::make_shared<bison::rmi::proxy::dynamic>(
        client_->instantiate("wish"_key, "__ToolSourceStub"_key).get());
    ASSERT_TRUE(proxy_->valid());
    host_ = std::make_unique<client_app_host>(*client_);
    // Not start()ed: the tests call run_logged() on their own thread, which
    // then is the worker thread. The stub commands finish long before the
    // progress dialog's delay, so no dialog is ever opened.
    worker_ = std::make_shared<common::command_worker>(*host_, "Running test");
  }

  void TearDown() override {
    worker_.reset();
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  test_source make_source(std::string tool, std::vector<std::string> launcher) {
    return test_source{proxy_, worker_, std::move(tool), std::move(launcher)};
  }

  /// The calls the stub form received for @p method, in order.
  static std::vector<dynamic> calls_to(const std::string& method) {
    std::vector<dynamic> out;
    for (auto& c : *recorded_calls().rlock())
      if (c.method == method)
        out.push_back(c.args);
    return out;
  }

  memory_server_transport transport_;
  std::unique_ptr<wish::server> srv_;
  std::unique_ptr<bison::rmi::client> client_;
  std::shared_ptr<bison::rmi::proxy::dynamic> proxy_;
  std::unique_ptr<client_app_host> host_;
  std::shared_ptr<common::command_worker> worker_;
};

TEST_F(ToolSourceTest, RunLoggedTracesTheCommandToTheConsole) {
  auto source = make_source("printf", {"printf"});
  auto r = source.run_logged({"%s\n", "hello world"});
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.stdout_text, "hello world\n");

  auto logs = calls_to("append_command_log");
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_EQ(logs[0].as<std::string>("command"_key), "printf %s hello world") << "the caption is one line";
  EXPECT_EQ(logs[0].as<int32_t>("exit_code"_key), 0);
  EXPECT_TRUE(logs[0].as<bool>("ok"_key));
  EXPECT_EQ(logs[0].as<std::string>("output"_key), "hello world");
}

TEST_F(ToolSourceTest, AFailedCommandIsTracedWithItsErrorText) {
  auto source = make_source("sh", {"sh", "-c"});
  auto r = source.run_logged({"echo progress; echo oops >&2; exit 3"});
  EXPECT_EQ(r.exit_code, 3);

  auto logs = calls_to("append_command_log");
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_EQ(logs[0].as<int32_t>("exit_code"_key), 3);
  EXPECT_FALSE(logs[0].as<bool>("ok"_key));
  EXPECT_EQ(logs[0].as<std::string>("output"_key), "oops");
}

TEST_F(ToolSourceTest, CaptionAndStdinCanBeOverridden) {
  auto source = make_source("cat", {"cat"});
  common::run_options options;
  options.caption = "cat <masked>";
  options.stdin_text = "secret";
  auto r = source.run_logged({}, options);
  EXPECT_EQ(r.stdout_text, "secret");

  auto logs = calls_to("append_command_log");
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_EQ(logs[0].as<std::string>("command"_key), "cat <masked>");
}

TEST_F(ToolSourceTest, WithoutAToolNameTheArgvIsTheWholeCommand) {
  auto source = make_source({}, {});
  EXPECT_EQ(source.caption({"printf", "a\tb"}), "printf a b");
  EXPECT_TRUE(source.run_logged({"printf", "x"}).ok());
}

TEST_F(ToolSourceTest, ReportSendsTheOutcomeToCommandResult) {
  auto source = make_source("tool", {"tool"});
  EXPECT_TRUE(source.report("install x", "packages", false, "it broke"));
  EXPECT_TRUE(source.report("refresh", {}, true, "ignored on success"));

  auto results = calls_to("command_result");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].as<std::string>("command"_key), "install x");
  EXPECT_EQ(results[0].as<std::string>("scope"_key), "packages");
  EXPECT_FALSE(results[0].as<bool>("ok"_key));
  EXPECT_EQ(results[0].as<std::string>("output"_key), "it broke");
  EXPECT_EQ(results[1].findField<std::string>("scope"_key), nullptr) << "an empty scope is omitted";
  EXPECT_TRUE(results[1].as<bool>("ok"_key));
  EXPECT_EQ(results[1].as<std::string>("output"_key), "");
}

TEST_F(ToolSourceTest, PushRowsSendsOneEntryPerTabSeparatedLine) {
  auto source = make_source("printf", {"printf"});
  source.push_rows(
      {"a\tb\n\nc\n"}, 2, "rows"_key, "update_rows"_key, [](dynamic& e, const std::vector<std::string>& c) {
        e["x"_key] = c[0];
        e["y"_key] = c[1];
      });

  auto pushes = calls_to("update_rows");
  ASSERT_EQ(pushes.size(), 1u);
  auto* rows = pushes[0].findField<dynamic_ptr>("rows"_key);
  ASSERT_NE(rows, nullptr);
  ASSERT_TRUE(*rows);
  std::vector<std::pair<std::string, std::string>> got;
  // An array's entries are its dynamic_ptr fields (it also carries
  // bookkeeping fields such as its class).
  (*rows)->forEach([&](bison::key_t, const field& f) {
    if (!f.is<dynamic_ptr>())
      return;
    auto& e = *f.as<dynamic_ptr>();
    got.emplace_back(e.as<std::string>("x"_key), e.as<std::string>("y"_key));
  });
  EXPECT_EQ(got, (std::vector<std::pair<std::string, std::string>>{{"a", "b"}, {"c", ""}}))
      << "blank lines are skipped; short rows are padded";
}

TEST_F(ToolSourceTest, PushRowsOfAFailedCommandPushesAnEmptyArray) {
  auto source = make_source("false", {"false"});
  source.push_rows({}, 1, "rows"_key, "update_rows"_key, [](dynamic&, const std::vector<std::string>&) {});

  auto pushes = calls_to("update_rows");
  ASSERT_EQ(pushes.size(), 1u);
  auto* rows = pushes[0].findField<dynamic_ptr>("rows"_key);
  ASSERT_NE(rows, nullptr);
  size_t n = 0;
  (*rows)->forEach([&](bison::key_t, const field& f) { n += f.is<dynamic_ptr>() ? 1 : 0; });
  EXPECT_EQ(n, 0u);
}

TEST(ToolSourceNoFormTest, CallWithoutAFormFails) {
  test_source source{nullptr, nullptr, "tool", {"printf"}};
  EXPECT_FALSE(source.call("append_command_log"_key, dynamic{}));
  // run() needs neither a form nor a worker (startup probes use it).
  EXPECT_EQ(source.run({"ok"}).stdout_text, "ok");
}

} // namespace
