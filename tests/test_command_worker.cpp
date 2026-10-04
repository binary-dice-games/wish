// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "modules/common/command_worker.hpp"

#include <context/context.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};

 protected:
  void on_session_created(wish::context& s) override { last_session = &s; }
};

// Minimal wish_app_host: only instantiate() matters to command_worker (it
// creates its ProgressBox through it); the rest is never called.
class fake_host : public wish::wish_app_host {
 public:
  explicit fake_host(rmi::client& client) : client_(client) {}
  std::future<rmi::proxy::dynamic> instantiate(bison::key_t ns, bison::key_t klass, dynamic params) override {
    return client_.instantiate(ns, klass, std::move(params));
  }
  std::future<void> upload_file(const std::string&, const std::string&, wish::transfer_progress_callback) override {
    return {};
  }
  std::future<std::string> download_file(const std::string&, wish::transfer_progress_callback) override { return {}; }
  void keep_alive(rmi::proxy::dynamic&&) override {}
  void signal_done() override {}
  const std::vector<std::string>& app_args() const override { return args_; }
  bool read_console_line(std::string&) override { return false; }

 private:
  rmi::client& client_;
  std::vector<std::string> args_;
};

} // namespace

class CommandWorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<rmi::client>(transport_.connect());
    client_->connect();
    host_ = std::make_unique<fake_host>(*client_);
    worker_ = std::make_shared<wish::common::command_worker>(*host_, "Testing");
    worker_->start();
  }

  void TearDown() override {
    worker_->shutdown();
    worker_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  // The ProgressBox's top-level root ("" when none was ever instantiated).
  std::string box_root() const {
    for (const auto& [k, _] : srv_->last_session->ui_objects)
      if (k.rfind("__progress_box_", 0) == 0 && k.find('.') == std::string::npos)
        return k;
    return {};
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<rmi::client> client_;
  std::unique_ptr<fake_host> host_;
  std::shared_ptr<wish::common::command_worker> worker_;
};

// A step that finishes before the progress delay never opens the dialog.
TEST_F(CommandWorkerTest, QuickTaskOpensNoDialog) {
  std::promise<int> done;
  worker_->post([&] {
    done.set_value(worker_->run_task("Quick", [](wish::common::task_progress& p) {
      p.report(1, 2);
      return 42;
    }));
  });
  auto fut = done.get_future();
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(fut.get(), 42);
  EXPECT_TRUE(box_root().empty());
}

// A slow step opens the dialog with a determinate bar carrying the reported
// fraction and detail, captioned by the step.
TEST_F(CommandWorkerTest, SlowTaskShowsDeterminateProgress) {
  std::promise<void> reported, checked;
  auto checked_fut = checked.get_future();
  worker_->post([&] {
    worker_->run_task("Uploading a.bin", [&](wish::common::task_progress& p) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      p.report(1, 4, "1 B / 4 B");
      reported.set_value();
      checked_fut.wait(); // keep the dialog up while the test looks at it.
    });
  });
  ASSERT_EQ(reported.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);

  const std::string root = box_root();
  ASSERT_FALSE(root.empty()) << "the dialog opens once the delay has passed";
  auto& objects = srv_->last_session->ui_objects;
  EXPECT_EQ(objects.at(root + ".vbox.command")->as<std::string>("text"_key), "Uploading a.bin");
  EXPECT_FLOAT_EQ(objects.at(root + ".vbox.bar")->as<float>("value"_key), 0.25f);
  EXPECT_EQ(objects.at(root + ".vbox.bar")->as<std::string>("label"_key), "1 B / 4 B");
  checked.set_value();
}

// cancel() is visible to the running step, and is consumed when run_task()
// returns so the rest of the job (and the next job) runs normally.
TEST_F(CommandWorkerTest, CancelIsSeenByTheStepAndConsumedAfterIt) {
  std::promise<void> started;
  std::promise<std::pair<bool, bool>> result;
  worker_->post([&] {
    bool seen = worker_->run_task("Long", [&](wish::common::task_progress& p) {
      started.set_value();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!p.cancelled() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return p.cancelled();
    });
    bool after = worker_->run_task("Next", [](wish::common::task_progress& p) { return p.cancelled(); });
    result.set_value({seen, after});
  });
  ASSERT_EQ(started.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready);
  worker_->cancel();

  auto fut = result.get_future();
  ASSERT_EQ(fut.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  auto [seen, after] = fut.get();
  EXPECT_TRUE(seen);
  EXPECT_FALSE(after);
}
