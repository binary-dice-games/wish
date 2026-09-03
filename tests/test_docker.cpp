// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

struct fake_container {
  std::string id;
  std::string name;
  std::string image;
  std::string state;
  std::string status;
  std::string ports;
  std::string created;
};

// Builds the `{ containers: [{id,name,image,state,status,ports,created}] }`
// shape update_containers expects -- see docker.hpp's do_update_containers
// doc comment.
dynamic make_containers_args(const std::vector<fake_container>& cs) {
  dynamic args;
  dynamic arr;
  size_t i = 0;
  for (auto& c : cs) {
    auto e = std::make_shared<dynamic>();
    (*e)["id"_key] = c.id;
    (*e)["name"_key] = c.name;
    (*e)["image"_key] = c.image;
    (*e)["state"_key] = c.state;
    (*e)["status"_key] = c.status;
    (*e)["ports"_key] = c.ports;
    (*e)["created"_key] = c.created;
    arr[i++] = dynamic_ptr{e};
  }
  args["containers"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

} // namespace

// ── Local (non-RMI) fixture ─────────────────────────────────────────────────

class DockerLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(DockerLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "DockerFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "DockerFrontend"_key);
}

// ── Session-capturing server ───────────────────────────────────────────────

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

// The windows this form registers are keyed "__docker_N" (the Containers
// main root) and "__docker_N_<suffix>" for images/volumes/networks/logs/
// inspect -- find the bare main root, mirroring test_git.cpp's
// find_form_root().
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__docker_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__docker_N_<suffix>": a second '_' after the numeric index.
    if (k.find('_', 9) != std::string::npos)
      continue;
    return k;
  }
  return {};
}

static std::string find_root_with_prefix(const wish::name_map& objects, const std::string& prefix) {
  for (const auto& [k, _] : objects)
    if (k.rfind(prefix, 0) == 0 && k.find('.') == std::string::npos)
      return k;
  return {};
}

class DockerRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "DockerFrontend"_key).get());
    ASSERT_TRUE(proxy_->valid());
    root_ = find_form_root(srv_->last_session->ui_objects);
    ASSERT_FALSE(root_.empty());
  }

  void TearDown() override {
    proxy_.reset();
    client_->disconnect();
    client_.reset();
    srv_->stop();
    srv_.reset();
  }

  dynamic call(bison::key_t method, dynamic args) {
    return proxy_->call(method, std::move(args)).get();
  }

  size_t row_count(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return 0;
    auto* cf = it->second->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return 0;
    return (*cf)->size();
  }

  static dynamic_ptr nth_child(const dynamic_ptr& parent, size_t index) {
    if (!parent)
      return nullptr;
    auto* cf = parent->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return nullptr;
    auto& f = (*cf)->at(index);
    return f.is<dynamic_ptr>() ? f.as<dynamic_ptr>() : nullptr;
  }

  static bison::key_t element_id(const dynamic_ptr& elem) {
    return elem ? elem->as<bison::key_t>("__wish_id"_key) : bison::key_t{};
  }

  dynamic_ptr container_row(size_t index) const {
    auto it = srv_->last_session->ui_objects.find(root_ + ".vbox.table");
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // __wish_id of a row's MenuItem, found by label (robust to the
  // state-dependent item ordering / separator slots in add_container_row()).
  bison::key_t menu_item_id(size_t row, const std::string& label) const {
    auto menu = nth_child(container_row(row), 5);
    if (!menu)
      return {};
    auto* cf = menu->findField<dynamic_ptr>("children"_key);
    if (!cf || !*cf)
      return {};
    bison::key_t found{};
    (*cf)->forEach([&](bison::key_t, const field& f) {
      if (found.id || !f.is<dynamic_ptr>())
        return;
      auto item = f.as<dynamic_ptr>();
      if (!item)
        return;
      auto* lf = item->findField<std::string>("label"_key);
      if (lf && *lf == label)
        found = item->as<bison::key_t>("__wish_id"_key);
    });
    return found;
  }

  std::string row_status_text(size_t row) const {
    auto cell = nth_child(container_row(row), 2);
    return cell ? cell->as<std::string>("text"_key) : std::string{};
  }
  bool row_visible(size_t row) const {
    auto r = container_row(row);
    return r ? r->as<bool>("visible"_key) : false;
  }

  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }
  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }

  static void wait_for(const bool& flag) {
    auto t0 = std::chrono::steady_clock::now();
    while (!flag && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Widget id of a toolbar / table element addressable by dot-path.
  bison::key_t id_at(const std::string& dot_path) const {
    auto it = srv_->last_session->ui_objects.find(root_ + "." + dot_path);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

TEST_F(DockerRmiTest, InstantiationBuildsContainersWindow) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.toolbar.btn_refresh"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + ".vbox.status"));
}

TEST_F(DockerRmiTest, UpdateContainersPopulatesTable) {
  call("update_containers"_key,
       make_containers_args({
           {"a1", "web", "nginx:latest", "running", "Up 3 hours", "80/tcp", "3 hours ago"},
           {"b2", "db", "postgres:16", "running", "Up 3 hours", "5432/tcp", "3 hours ago"},
           {"c3", "job", "busybox", "exited", "Exited (0) 1 hour ago", "", "5 hours ago"},
       }));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 3u);

  auto status = srv_->last_session->ui_objects.at(root_ + ".vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("3 containers"), std::string::npos);
  EXPECT_NE(status.find("2 running"), std::string::npos);
}

TEST_F(DockerRmiTest, RebuildIsIdempotentInRowCount) {
  auto args = make_containers_args({{"a1", "web", "nginx", "running", "Up", "", "now"}});
  call("update_containers"_key, args.clone());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 1u);
  call("update_containers"_key, args.clone());
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 1u);
}

TEST_F(DockerRmiTest, RunningRowMenuHasStopStoppedRowHasStart) {
  call("update_containers"_key,
       make_containers_args({
           {"a1", "web", "nginx", "running", "Up", "", "now"},
           {"c3", "job", "busybox", "exited", "Exited (0)", "", "1h"},
       }));
  EXPECT_FALSE(menu_item_id(0, "Stop...").id == 0);
  EXPECT_FALSE(menu_item_id(1, "Start").id == 0);
  EXPECT_TRUE(menu_item_id(1, "Stop...").id == 0);
}

TEST_F(DockerRmiTest, RemoveClickShowsConfirmDialogInsteadOfEmitting) {
  call("update_containers"_key,
       make_containers_args({{"a1", "web", "nginx", "running", "Up", "", "now"}}));

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "container_action_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire(menu_item_id(0, "Remove..."), "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got) << "remove must be held back pending confirmation";

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto msg = srv_->last_session->ui_objects.at(confirm_root + ".body.message")->as<std::string>("text"_key);
  EXPECT_NE(msg.find("web"), std::string::npos);
}

TEST_F(DockerRmiTest, ConfirmRemoveYesEmitsContainerActionRequested) {
  call("update_containers"_key,
       make_containers_args({{"a1", "web", "nginx", "running", "Up", "", "now"}}));
  fire(menu_item_id(0, "Remove..."), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto yes_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "container_action_requested"_key) {
      got = true;
      captured = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(confirm_root, yes_id, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("id"_key), "a1");
  EXPECT_EQ(captured.as<std::string>("action"_key), "remove");
}

TEST_F(DockerRmiTest, ConfirmRemoveNoCancelsWithoutEmitting) {
  call("update_containers"_key,
       make_containers_args({{"a1", "web", "nginx", "running", "Up", "", "now"}}));
  fire(menu_item_id(0, "Remove..."), "clicked"_key);

  std::string confirm_root = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(confirm_root.empty());
  auto no_id = srv_->last_session->ui_objects.at(confirm_root + ".buttons.btn1")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "container_action_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire_at(confirm_root, no_id, "clicked"_key);
  wait_for(got);
  EXPECT_FALSE(got);
}

TEST_F(DockerRmiTest, ReversibleActionFiresImmediately) {
  call("update_containers"_key,
       make_containers_args({{"c3", "job", "busybox", "exited", "Exited (0)", "", "1h"}}));

  bool got = false;
  dynamic captured;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "container_action_requested"_key) {
      got = true;
      captured = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };

  fire(menu_item_id(0, "Start"), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(captured.as<std::string>("action"_key), "start");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(DockerRmiTest, RefreshButtonEmitsRefreshRequested) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "refresh_requested"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire(id_at("vbox.toolbar.btn_refresh"), "clicked"_key);
  wait_for(got);
  EXPECT_TRUE(got);
}

TEST_F(DockerRmiTest, StateComboFilterHidesNonMatchingRows) {
  call("update_containers"_key,
       make_containers_args({
           {"a1", "web", "nginx", "running", "Up", "", "now"},
           {"c3", "job", "busybox", "exited", "Exited (0)", "", "1h"},
       }));
  EXPECT_TRUE(row_visible(0));
  EXPECT_TRUE(row_visible(1));

  dynamic p;
  p["value"_key] = int32_t{1}; // "Running"
  fire(id_at("vbox.toolbar.state"), "changed"_key, std::move(p));
  EXPECT_TRUE(row_visible(0));
  EXPECT_FALSE(row_visible(1));
}

TEST_F(DockerRmiTest, TextFilterMatchesNameAndImage) {
  call("update_containers"_key,
       make_containers_args({
           {"a1", "web", "nginx:latest", "running", "Up", "", "now"},
           {"b2", "cache", "redis:7", "running", "Up", "", "now"},
       }));
  dynamic p;
  p["value"_key] = std::string{"redis"};
  fire(id_at("vbox.toolbar.filter"), "changed"_key, std::move(p));
  EXPECT_FALSE(row_visible(0));
  EXPECT_TRUE(row_visible(1));
}

TEST_F(DockerRmiTest, CommandResultShowsFailureInStatus) {
  dynamic args;
  args["command"_key] = std::string{"stop"};
  args["ok"_key] = false;
  args["output"_key] = std::string{"No such container: zzz"};
  call("command_result"_key, std::move(args));
  auto status = srv_->last_session->ui_objects.at(root_ + ".vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(status.find("stop failed"), std::string::npos);
  EXPECT_NE(status.find("No such container"), std::string::npos);
}

// ── Images / Volumes / Networks windows ────────────────────────────────────

namespace {
// Generic `{ <key>: [ {field:value, ...}, ... ] }` builder.
dynamic make_list_args(const std::string& key, const std::vector<std::vector<std::pair<std::string, std::string>>>& rows) {
  dynamic args;
  dynamic arr;
  size_t i = 0;
  for (auto& row : rows) {
    auto e = std::make_shared<dynamic>();
    for (auto& [k, v] : row)
      (*e)[bison::key_t{k}] = v;
    arr[i++] = dynamic_ptr{e};
  }
  args[bison::key_t{key}] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}
} // namespace

TEST_F(DockerRmiTest, InstantiationBuildsAllFourWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_images"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_volumes"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_networks"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_images"}));
}

TEST_F(DockerRmiTest, UpdateImagesPopulatesTable) {
  call("update_images"_key,
       make_list_args("images", {
           {{"id", "abc123"}, {"repository", "nginx"}, {"tag", "latest"}, {"created", "2 weeks ago"}, {"size", "187MB"}},
           {{"id", "def456"}, {"repository", "<none>"}, {"tag", "<none>"}, {"created", "6 days ago"}, {"size", "1.1GB"}},
       }));
  EXPECT_EQ(row_count(root_ + "_images.vbox.table"), 2u);
  auto st = srv_->last_session->ui_objects.at(root_ + "_images.vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(st.find("2 images"), std::string::npos);
}

TEST_F(DockerRmiTest, UpdateVolumesAndNetworksPopulateTables) {
  call("update_volumes"_key,
       make_list_args("volumes", {{{"name", "pgdata"}, {"driver", "local"}, {"mountpoint", "/x/pgdata"}}}));
  EXPECT_EQ(row_count(root_ + "_volumes.vbox.table"), 1u);

  call("update_networks"_key,
       make_list_args("networks", {
           {{"id", "n1"}, {"name", "bridge"}, {"driver", "bridge"}, {"scope", "local"}},
           {{"id", "n2"}, {"name", "myapp_default"}, {"driver", "bridge"}, {"scope", "local"}},
       }));
  EXPECT_EQ(row_count(root_ + "_networks.vbox.table"), 2u);
}

// menu item id lookup for a non-Containers window: <root_suffix> selects the
// window, `row`/`label` as before.
static bison::key_t menu_id_in(
    const wish::name_map& objects, const std::string& table_path, size_t row, const std::string& label) {
  auto it = objects.find(table_path);
  if (it == objects.end())
    return {};
  auto* tcf = it->second->findField<dynamic_ptr>("children"_key);
  if (!tcf || !*tcf)
    return {};
  auto row_ptr = (*tcf)->at(row).is<dynamic_ptr>() ? (*tcf)->at(row).as<dynamic_ptr>() : nullptr;
  if (!row_ptr)
    return {};
  auto* rcf = row_ptr->findField<dynamic_ptr>("children"_key);
  if (!rcf || !*rcf)
    return {};
  // last cell is the MenuButton
  size_t last = (*rcf)->size() - 1;
  auto menu = (*rcf)->at(last).as<dynamic_ptr>();
  auto* mcf = menu->findField<dynamic_ptr>("children"_key);
  bison::key_t found{};
  if (mcf && *mcf)
    (*mcf)->forEach([&](bison::key_t, const field& f) {
      if (found.id || !f.is<dynamic_ptr>() || !f.as<dynamic_ptr>())
        return;
      auto* lf = f.as<dynamic_ptr>()->findField<std::string>("label"_key);
      if (lf && *lf == label)
        found = f.as<dynamic_ptr>()->as<bison::key_t>("__wish_id"_key);
    });
  return found;
}

TEST_F(DockerRmiTest, ImageRemoveConfirmsThenEmitsImageActionRequested) {
  call("update_images"_key,
       make_list_args("images", {{{"id", "abc123"}, {"repository", "nginx"}, {"tag", "latest"}, {"created", "x"}, {"size", "1MB"}}}));

  auto rm = menu_id_in(srv_->last_session->ui_objects, root_ + "_images.vbox.table", 0, "Remove...");
  ASSERT_NE(rm.id, 0u);
  // The MenuItem belongs to the images window's tree -> its handler is the
  // form registered under the images root.
  fire_at(root_ + "_images", rm, "clicked"_key);

  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "image_action_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(cr, yes, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("id"_key), "abc123");
  EXPECT_EQ(cap.as<std::string>("action"_key), "remove");
}

TEST_F(DockerRmiTest, VolumeRemoveEmitsWithNameField) {
  call("update_volumes"_key,
       make_list_args("volumes", {{{"name", "pgdata"}, {"driver", "local"}, {"mountpoint", "/x"}}}));
  auto rm = menu_id_in(srv_->last_session->ui_objects, root_ + "_volumes.vbox.table", 0, "Remove...");
  ASSERT_NE(rm.id, 0u);
  fire_at(root_ + "_volumes", rm, "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "volume_action_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(cr, yes, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("name"_key), "pgdata");
}

TEST_F(DockerRmiTest, BuiltinNetworkHasNoRemoveItem) {
  call("update_networks"_key,
       make_list_args("networks", {
           {{"id", "n1"}, {"name", "bridge"}, {"driver", "bridge"}, {"scope", "local"}},
           {{"id", "n2"}, {"name", "myapp_default"}, {"driver", "bridge"}, {"scope", "local"}},
       }));
  EXPECT_EQ(menu_id_in(srv_->last_session->ui_objects, root_ + "_networks.vbox.table", 0, "Remove...").id, 0u);
  EXPECT_NE(menu_id_in(srv_->last_session->ui_objects, root_ + "_networks.vbox.table", 1, "Remove...").id, 0u);
}

TEST_F(DockerRmiTest, PruneImagesConfirmsThenEmitsScope) {
  auto btn = srv_->last_session->ui_objects.at(root_ + "_images.vbox.toolbar.btn_prune")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_images", btn, "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto yes = srv_->last_session->ui_objects.at(cr + ".buttons.btn0")->as<bison::key_t>("__wish_id"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "prune_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire_at(cr, yes, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("scope"_key), "images");
}

TEST_F(DockerRmiTest, PullButtonEmitsPullImageRequestedFromInlineField) {
  auto in = srv_->last_session->ui_objects.at(root_ + "_images.vbox.toolbar.pull_ref")->as<bison::key_t>("__wish_id"_key);
  dynamic p;
  p["value"_key] = std::string{"redis:7"};
  fire_at(root_ + "_images", in, "changed"_key, std::move(p));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "pull_image_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  auto btn = srv_->last_session->ui_objects.at(root_ + "_images.vbox.toolbar.btn_pull")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_images", btn, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("ref"_key), "redis:7");
}

TEST_F(DockerRmiTest, CommandResultScopeRoutesToRightWindowStatus) {
  dynamic args;
  args["command"_key] = std::string{"pull redis:7"};
  args["scope"_key] = std::string{"images"};
  args["ok"_key] = true;
  args["output"_key] = std::string{};
  call("command_result"_key, std::move(args));
  auto st = srv_->last_session->ui_objects.at(root_ + "_images.vbox.status")->as<std::string>("text"_key);
  EXPECT_NE(st.find("pull redis:7"), std::string::npos);
}

TEST_F(DockerRmiTest, ClosingAnyWindowEmitsClosed) {
  bool got = false;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "closed"_key)
      got = true;
    if (prev)
      prev(id, event, std::move(payload));
  };
  auto win = srv_->last_session->ui_objects.at(root_ + "_volumes")->as<bison::key_t>("__wish_id"_key);
  fire_at(root_ + "_volumes", win, "closed"_key);
  wait_for(got);
  EXPECT_TRUE(got);
}

// ── Logs / Inspect windows ────────────────────────────────────────────────

TEST_F(DockerRmiTest, InstantiationBuildsLogsAndInspectWindows) {
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_logs.vbox.table"));
  EXPECT_TRUE(srv_->last_session->ui_objects.count(root_ + "_inspect.vbox.table"));
  EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + "_logs"}));
}

TEST_F(DockerRmiTest, LogsMenuActionSetsTargetAndEmitsLogsRequested) {
  call("update_containers"_key,
       make_containers_args({{"cid42", "web", "nginx", "running", "Up", "", "now"}}));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "logs_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  fire(menu_item_id(0, "Logs"), "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("id"_key), "cid42");
  EXPECT_FALSE(cap.as<bool>("follow"_key));

  auto target = srv_->last_session->ui_objects.at(root_ + "_logs.vbox.toolbar.target")->as<std::string>("text"_key);
  EXPECT_NE(target.find("web"), std::string::npos);
}

TEST_F(DockerRmiTest, UpdateLogsSplitsTextAndGuardsAgainstStaleTarget) {
  call("update_containers"_key,
       make_containers_args({{"cid42", "web", "nginx", "running", "Up", "", "now"}}));
  fire(menu_item_id(0, "Logs"), "clicked"_key); // sets open_logs_id_ = "cid42"

  dynamic ok;
  ok["container_id"_key] = std::string{"cid42"};
  ok["title"_key] = std::string{"logs: cid42"};
  ok["text"_key] = std::string{"line one\nline two\nline three"};
  call("update_logs"_key, std::move(ok));
  EXPECT_EQ(row_count(root_ + "_logs.vbox.table"), 3u);

  // A response for a container the user is no longer viewing is discarded.
  dynamic stale;
  stale["container_id"_key] = std::string{"other"};
  stale["title"_key] = std::string{"stale"};
  stale["text"_key] = std::string{"a\nb\nc\nd\ne"};
  call("update_logs"_key, std::move(stale));
  EXPECT_EQ(row_count(root_ + "_logs.vbox.table"), 3u);
}

TEST_F(DockerRmiTest, LogsFollowCheckboxReEmitsWithFollowTrue) {
  call("update_containers"_key,
       make_containers_args({{"cid42", "web", "nginx", "running", "Up", "", "now"}}));
  fire(menu_item_id(0, "Logs"), "clicked"_key);

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "logs_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  auto follow = srv_->last_session->ui_objects.at(root_ + "_logs.vbox.toolbar.follow")->as<bison::key_t>("__wish_id"_key);
  dynamic p;
  p["value"_key] = true;
  fire_at(root_ + "_logs", follow, "changed"_key, std::move(p));
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_TRUE(cap.as<bool>("follow"_key));
}

TEST_F(DockerRmiTest, InspectMenuActionEmitsAndUpdateInspectFills) {
  call("update_networks"_key,
       make_list_args("networks", {{{"id", "netid9"}, {"name", "mynet"}, {"driver", "bridge"}, {"scope", "local"}}}));

  bool got = false;
  dynamic cap;
  auto prev = std::move(srv_->last_session->emit_event);
  srv_->last_session->emit_event = [&](bison::key_t id, bison::key_t event, dynamic payload) {
    if (event == "inspect_requested"_key) {
      got = true;
      cap = payload.clone();
    }
    if (prev)
      prev(id, event, std::move(payload));
  };
  auto ins = menu_id_in(srv_->last_session->ui_objects, root_ + "_networks.vbox.table", 0, "Inspect");
  ASSERT_NE(ins.id, 0u);
  fire_at(root_ + "_networks", ins, "clicked"_key);
  wait_for(got);
  ASSERT_TRUE(got);
  EXPECT_EQ(cap.as<std::string>("kind"_key), "network");
  EXPECT_EQ(cap.as<std::string>("id"_key), "netid9");

  dynamic resp;
  resp["target_id"_key] = std::string{"netid9"};
  resp["kind"_key] = std::string{"network"};
  resp["title"_key] = std::string{"network: netid9"};
  resp["text"_key] = std::string{"[\n  { \"Name\": \"mynet\" }\n]"};
  call("update_inspect"_key, std::move(resp));
  EXPECT_EQ(row_count(root_ + "_inspect.vbox.table"), 3u);
}
