// MIT License © 2026 Binary Dice Games
/// @file store_session.hpp
/// @brief Test helper: a wish server with persistent stores (a temp store
///        dir and `local_auth_module`) and one form opened as a named user,
///        for tests of the settings a module remembers across sessions.
#pragma once

#include <auth/local_auth_module.hpp>
#include <context/context.hpp>
#include <server/server.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"
#include "src/rmi/shared/ids.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace wish_test {

/// @brief See the file comment. One form at a time: open() closes the
///        previous session first, so the next one reads what it saved.
class store_session {
 public:
  store_session() {
    srv_ = std::make_unique<capturing_server>(transport_, std::make_unique<bdg::wish::null_renderer>());
    srv_->set_store_dir(dir_);
    srv_->start(std::make_shared<bdg::wish::local_auth_module>());
  }

  ~store_session() {
    close();
    srv_->stop();
    srv_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /// @brief Connects as @p user and instantiates form class @p klass.
  void open(bdg::bison::key_t klass, const std::string& user = "alice") {
    using namespace bdg::bison;
    close();
    client_ = std::make_unique<rmi::client>(transport_.connect());
    dynamic params;
    params["username"_key] = user;
    client_->connect(std::move(params));
    proxy_.emplace(client_->instantiate("wish"_key, klass).get());
    ASSERT_TRUE(proxy_->valid());
  }

  void close() {
    proxy_.reset();
    if (client_)
      client_->disconnect();
    client_.reset();
  }

  bdg::bison::rmi::proxy::dynamic& proxy() {
    return *proxy_;
  }

  /// @brief The current session (the last one opened).
  bdg::wish::context& session() {
    return *srv_->last_session;
  }

  /// @brief An element whose path ends with "." + @p suffix (e.g.
  ///        "toolbar.chk_follow"); fails the test when there is none.
  bdg::wish::ui_element_ptr element(const std::string& suffix) {
    return find(suffix).second;
  }

  /// @brief Delivers widget event @p event from element(@p suffix) to its
  ///        form, the way the render loop does.
  void fire(const std::string& suffix, bdg::bison::key_t event, const bdg::bison::dynamic& payload = {}) {
    using namespace bdg::bison;
    auto [path, el] = find(suffix);
    ASSERT_TRUE(el) << suffix;
    // The form's root is the first path component; its handler gets the event.
    auto h = session().top_level_handlers.find(bdg::bison::key_t{path.substr(0, path.find('.'))});
    ASSERT_NE(h, session().top_level_handlers.end()) << path;
    h->second->on_event(el->as<bdg::bison::key_t>("__wish_id"_key), event, payload);
  }

  /// @brief The user store entry @p name of the current session.
  std::optional<bdg::bison::dynamic> entry(const std::string& name) {
    return session().user_store->get(name);
  }

 private:
  std::pair<std::string, bdg::wish::ui_element_ptr> find(const std::string& suffix) {
    for (const auto& [path, el] : session().ui_objects)
      if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 &&
          path[path.size() - suffix.size() - 1] == '.')
        return {path, el};
    ADD_FAILURE() << "no element " << suffix;
    return {};
  }

  class capturing_server : public bdg::wish::server {
   public:
    using bdg::wish::server::server;
    bdg::wish::context* last_session{nullptr};

   protected:
    void on_session_created(bdg::wish::context& s) override {
      last_session = &s;
    }
  };

  std::filesystem::path dir_ = std::filesystem::temp_directory_path() /
      ("wish_store_session_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + "_" +
       std::to_string(bdg::bison::rmi::shared::generate_id().id));
  bdg::bison::rmi::transport::memory_server_transport transport_;
  std::unique_ptr<capturing_server> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<bdg::bison::rmi::proxy::dynamic> proxy_;
};

} // namespace wish_test
