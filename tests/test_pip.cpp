// MIT License © 2026 Binary Dice Games
#include <gtest/gtest.h>

#include "session_event_recorder.hpp"

#include <server/registry.hpp>
#include <server/server.hpp>
#include <context/context.hpp>
#include <ui/ui_root.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/rmi.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;
using namespace bdg::bison::rmi::transport;

namespace {

// Generic `{ <key>: [ {field:value, ...}, ... ] }` builder -- the shape every
// update_* method expects (see pip.hpp's do_update_* doc comments).
dynamic make_list_args(
    const std::string& key, const std::vector<std::vector<std::pair<std::string, std::string>>>& rows) {
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

// ── Local (non-RMI) fixture ─────────────────────────────────────────────────

class PipLocalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bdg::wish::register_all();
  }
};

TEST_F(PipLocalTest, CanBeInstantiated) {
  auto obj = dynamic::instantiate("wish"_key, "PipFrontend"_key);
  auto* cls = obj.findField(dynamic::CLASS);
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(cls->as<bison::key_t>(), "PipFrontend"_key);
}

// ── Session-capturing server ───────────────────────────────────────────────

class SessionCapturingServer : public wish::server {
 public:
  SessionCapturingServer(server_transport_iface& t, std::unique_ptr<wish::renderer> r)
      : wish::server(t, std::move(r)) {}
  wish::context* last_session{nullptr};
  /// Every event the session emits (see session_event_recorder.hpp).
  std::shared_ptr<session_event_recorder> events = std::make_shared<session_event_recorder>();

 protected:
  void on_session_created(wish::context& s) override {
    last_session = &s;
    session_event_recorder::attach(events, s);
  }
};

// The windows this form registers are keyed "__pip_N" (the Packages main
// root) and "__pip_N_<suffix>" for versions/details/console --
// find the bare main root (mirrors test_helm.cpp).
static std::string find_form_root(const wish::name_map& objects) {
  for (const auto& [k, _] : objects) {
    if (k.rfind("__pip_", 0) != 0 || k.find('.') != std::string::npos)
      continue;
    // Reject "__pip_N_<suffix>": a second '_' after the numeric index
    // ("__pip_" is 6 chars).
    if (k.find('_', 6) != std::string::npos)
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

class PipRmiTest : public ::testing::Test {
  using proxy_t = bdg::bison::rmi::proxy::dynamic;

 protected:
  void SetUp() override {
    srv_ = std::make_unique<SessionCapturingServer>(transport_, std::make_unique<wish::null_renderer>());
    srv_->start();
    client_ = std::make_unique<bdg::bison::rmi::client>(transport_.connect());
    client_->connect();
    proxy_.emplace(client_->instantiate("wish"_key, "PipFrontend"_key).get());
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

  // file_path of the TextEditor at @p path ("" if missing/unset).
  std::string editor_file(const std::string& path) const {
    auto it = srv_->last_session->ui_objects.find(path);
    if (it == srv_->last_session->ui_objects.end())
      return {};
    auto* f = it->second->findField<std::string>("file_path"_key);
    return f ? *f : std::string{};
  }

  // Contents of the sandbox file the TextEditor at @p path displays.
  std::string editor_text(const std::string& path) const {
    auto rel = editor_file(path);
    if (rel.empty())
      return {};
    std::ifstream in(srv_->last_session->resource_dir / rel, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
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

  dynamic_ptr row_in(const std::string& table_path, size_t index) const {
    auto it = srv_->last_session->ui_objects.find(table_path);
    if (it == srv_->last_session->ui_objects.end())
      return nullptr;
    return nth_child(it->second, index);
  }

  // __wish_id of a row's MenuItem, found by label (robust to the
  // state-dependent item ordering / separator slots).
  bison::key_t menu_id_in(const std::string& table_path, size_t row, const std::string& label) const {
    auto row_ptr = row_in(table_path, row);
    if (!row_ptr)
      return {};
    auto* rcf = row_ptr->findField<dynamic_ptr>("children"_key);
    if (!rcf || !*rcf)
      return {};
    size_t last = (*rcf)->size() - 1; // last cell is the MenuButton
    auto menu = (*rcf)->at(last).as<dynamic_ptr>();
    if (!menu)
      return {};
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

  bool row_visible(const std::string& table_path, size_t row) const {
    auto r = row_in(table_path, row);
    return r ? r->as<bool>("visible"_key) : false;
  }

  void fire_at(const std::string& handler_root, bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    auto h = srv_->last_session->top_level_handlers.find(handler_root);
    ASSERT_NE(h, srv_->last_session->top_level_handlers.end());
    h->second->on_event(id, event, std::move(payload));
  }
  void fire(bison::key_t id, bison::key_t event, dynamic payload = dynamic{}) {
    fire_at(root_, id, event, std::move(payload));
  }

  bison::key_t id_at(const std::string& abs_path) const {
    auto it = srv_->last_session->ui_objects.find(abs_path);
    return it == srv_->last_session->ui_objects.end() ? bison::key_t{}
                                                      : it->second->as<bison::key_t>("__wish_id"_key);
  }

  memory_server_transport transport_;
  std::unique_ptr<SessionCapturingServer> srv_;
  std::unique_ptr<bdg::bison::rmi::client> client_;
  std::optional<proxy_t> proxy_;
  std::string root_;
};

// ── Shared snapshots ────────────────────────────────────────────────────────

namespace {

// requests is outdated, wish-abi is an editable install, six is neither.
dynamic three_packages(bool outdated_checked = true) {
  auto args = make_list_args(
      "packages",
      {
          {{"name", "requests"}, {"version", "2.31.0"}, {"latest", "2.34.2"}, {"location", ""}},
          {{"name", "six"}, {"version", "1.17.0"}, {"latest", ""}, {"location", ""}},
          {{"name", "wish-abi"}, {"version", "1.0.0"}, {"latest", ""}, {"location", "/src/wish/bindings/python"}},
      });
  args["outdated_checked"_key] = outdated_checked;
  return args;
}

dynamic versions_of(const std::string& name, const std::vector<std::string>& versions, const std::string& latest) {
  dynamic arr;
  size_t i = 0;
  for (auto& v : versions)
    arr[i++] = v;
  dynamic args;
  args["name"_key] = name;
  args["latest"_key] = latest;
  args["versions"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return args;
}

dynamic value_payload(const std::string& v) {
  dynamic p;
  p["value"_key] = v;
  return p;
}

dynamic bool_payload(bool v) {
  dynamic p;
  p["value"_key] = v;
  return p;
}

} // namespace

#define PIP_TEXT_AT(path) srv_->last_session->ui_objects.at(path)->as<std::string>("text"_key)

// ── Windows ─────────────────────────────────────────────────────────────────

TEST_F(PipRmiTest, InstantiationBuildsAllFourWindows) {
  auto& objects = srv_->last_session->ui_objects;
  EXPECT_TRUE(objects.count(root_ + ".vbox.toolbar.btn_refresh"));
  EXPECT_TRUE(objects.count(root_ + ".vbox.install_bar.btn_install"));
  EXPECT_TRUE(objects.count(root_ + ".vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_versions.vbox.table"));
  EXPECT_TRUE(objects.count(root_ + "_details.vbox.editor"));
  EXPECT_TRUE(objects.count(root_ + "_console.vbox.table"));
  for (const char* suffix : {"_versions", "_details", "_console"})
    EXPECT_TRUE(srv_->last_session->top_level_handlers.count(bison::key_t{root_ + suffix})) << suffix;
}

TEST_F(PipRmiTest, RegistersDefaultDockLayoutAndClosingTearsItDown) {
  const std::string dl = find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_");
  ASSERT_FALSE(dl.empty());
  EXPECT_EQ(srv_->last_session->ui_objects.at(dl)->class_key(), "DockSpaceViewport"_key);

  size_t since = srv_->events->mark();
  fire_at(root_ + "_versions", id_at(root_ + "_versions"), "closed"_key);
  EXPECT_TRUE(srv_->events->wait_for("closed"_key, since));
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__docklayout_").empty());
  EXPECT_FALSE(srv_->last_session->ui_objects.count(root_ + "_details"));
}

TEST_F(PipRmiTest, SetEnvironmentShowsInterpreterAndPipVersion) {
  dynamic args;
  args["text"_key] = std::string{"pip 25.1.1 from /venv/lib/pip (python 3.14)"};
  args["interpreter"_key] = std::string{"/venv/bin/python"};
  call("set_environment"_key, std::move(args));
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.env"), "/venv/bin/python: pip 25.1.1 from /venv/lib/pip (python 3.14)");
}

// ── Progress dialog ─────────────────────────────────────────────────────────

namespace {

dynamic progress(bool active, const std::string& command = {}, float phase = 0.0f,
                 const std::vector<std::string>& lines = {}) {
  dynamic arr;
  size_t i = 0;
  for (auto& line : lines)
    arr[i++] = line;
  dynamic a;
  a["active"_key] = active;
  a["command"_key] = command;
  a["phase"_key] = phase;
  a["lines"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(arr))};
  return a;
}

} // namespace

TEST_F(PipRmiTest, ProgressDialogIsAModalThatCollectsOutputAndClosesWhenIdle) {
  auto& objects = srv_->last_session->ui_objects;
  const std::string dlg = root_ + "_progress";
  EXPECT_FALSE(objects.count(dlg)) << "built on demand";
  EXPECT_FALSE(objects.count(root_ + ".vbox.progress")) << "nothing inline in the Packages window";

  call("set_progress"_key, progress(true, "pip install requests", 1.0f, {"Collecting requests"}));
  ASSERT_TRUE(objects.count(dlg));
  EXPECT_TRUE(objects.at(dlg)->as<bool>("modal"_key));
  EXPECT_EQ(PIP_TEXT_AT(dlg + ".vbox.command"), "pip install requests");
  EXPECT_EQ(row_count(dlg + ".vbox.table"), 2u) << "the command line + one output line";
  const float first = objects.at(dlg + ".vbox.bar")->as<float>("value"_key);
  EXPECT_LT(first, 0.0f) << "negative = indeterminate";

  call("set_progress"_key, progress(true, "pip install requests", 2.0f, {"Downloading requests", "Installing"}));
  EXPECT_EQ(row_count(dlg + ".vbox.table"), 4u) << "same command: no second header row";
  EXPECT_LT(objects.at(dlg + ".vbox.bar")->as<float>("value"_key), first) << "the phase animates the bar";
  call("set_progress"_key, progress(true, "pip list --format=json", 0.1f));
  EXPECT_EQ(row_count(dlg + ".vbox.table"), 5u) << "a new command adds its header row";

  // Idle: the modal is asked to close, and torn down once the renderer says so.
  call("set_progress"_key, progress(false));
  ASSERT_TRUE(objects.count(dlg));
  EXPECT_TRUE(objects.at(dlg)->as<bool>("__request_close__"_key));
  fire_at(dlg, id_at(dlg), "closed"_key);
  EXPECT_FALSE(objects.count(dlg));
  EXPECT_TRUE(objects.count(root_ + ".vbox.table")) << "only the dialog went away";
}

TEST_F(PipRmiTest, ProgressDialogStaysOpenOnFailureUntilClosed) {
  auto& objects = srv_->last_session->ui_objects;
  const std::string dlg = root_ + "_progress";
  call("set_progress"_key, progress(true, "pip install nope", 1.0f));

  dynamic failed;
  failed["command"_key] = std::string{"install nope"};
  failed["ok"_key] = false;
  failed["output"_key] = std::string{"ERROR: Could not find a version\nERROR: No matching distribution found for nope"};
  call("command_result"_key, std::move(failed));
  call("set_progress"_key, progress(false));
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status").find('\n'), std::string::npos) << "the status line stays one line";

  ASSERT_TRUE(objects.count(dlg));
  EXPECT_FALSE(objects.at(dlg)->findField<bool>("__request_close__"_key)) << "left open to show the error";
  EXPECT_NE(PIP_TEXT_AT(dlg + ".vbox.result").find("No matching distribution"), std::string::npos);
  EXPECT_TRUE(objects.at(dlg + ".vbox.result")->as<bool>("visible"_key));
  EXPECT_FALSE(objects.at(dlg + ".vbox.bar")->as<bool>("visible"_key));
  EXPECT_EQ(objects.at(dlg + ".vbox.btn_cancel")->as<std::string>("label"_key), "Close");

  size_t since = srv_->events->mark();
  fire_at(dlg, id_at(dlg + ".vbox.btn_cancel"), "clicked"_key);
  EXPECT_TRUE(objects.at(dlg)->as<bool>("__request_close__"_key));
  EXPECT_FALSE(srv_->events->wait_for("cancel_requested"_key, since, nullptr, std::chrono::milliseconds{100}))
      << "Close is not Cancel";
}

TEST_F(PipRmiTest, ProgressDialogCancelEmitsOnceAndClosesEvenThoughTheCommandFailed) {
  auto& objects = srv_->last_session->ui_objects;
  const std::string dlg = root_ + "_progress";
  call("set_progress"_key, progress(true, "pip install torch", 1.0f));

  size_t since = srv_->events->mark();
  fire_at(dlg, id_at(dlg + ".vbox.btn_cancel"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("cancel_requested"_key, since));
  EXPECT_EQ(PIP_TEXT_AT(dlg + ".vbox.command"), "Cancelling ...");

  dynamic failed;
  failed["command"_key] = std::string{"install torch"};
  failed["ok"_key] = false;
  failed["output"_key] = std::string{"cancelled"};
  call("command_result"_key, std::move(failed));
  call("set_progress"_key, progress(false));
  EXPECT_TRUE(objects.at(dlg)->as<bool>("__request_close__"_key));
}

TEST_F(PipRmiTest, ProgressDialogReopensWhenACommandStartsWhileItIsClosing) {
  auto& objects = srv_->last_session->ui_objects;
  const std::string dlg = root_ + "_progress";
  call("set_progress"_key, progress(true, "pip install a", 1.0f));
  call("set_progress"_key, progress(false));
  call("set_progress"_key, progress(true, "pip install b", 0.5f)); // before the renderer closed it
  fire_at(dlg, id_at(dlg), "closed"_key);
  ASSERT_TRUE(objects.count(dlg)) << "rebuilt for the new command";
  EXPECT_FALSE(objects.at(dlg)->findField<bool>("__request_close__"_key));
  call("set_progress"_key, progress(true, "pip install b", 0.7f));
  EXPECT_EQ(PIP_TEXT_AT(dlg + ".vbox.command"), "pip install b");
}

TEST_F(PipRmiTest, LookUpClearsAStaleValidationMessage) {
  call("update_packages"_key, three_packages(false));
  fire(id_at(root_ + ".vbox.install_bar.btn_versions"), "clicked"_key);
  EXPECT_NE(PIP_TEXT_AT(root_ + ".vbox.status").find("Enter a package name"), std::string::npos);
  fire(id_at(root_ + ".vbox.install_bar.spec"), "changed"_key, value_payload("sphinx"));
  fire(id_at(root_ + ".vbox.install_bar.btn_versions"), "clicked"_key);
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "3 packages");
}

// ── Packages ────────────────────────────────────────────────────────────────

TEST_F(PipRmiTest, UpdatePackagesPopulatesTableAndStatusCounts) {
  call("update_packages"_key, three_packages(false));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 3u);
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "3 packages") << "no outdated count before a check";

  call("update_packages"_key, three_packages(true));
  EXPECT_EQ(row_count(root_ + ".vbox.table"), 3u) << "a rebuild must replace, not append";
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "3 packages (1 outdated)");

  call("update_packages"_key, make_list_args("packages", {}));
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "No packages installed.");
}

TEST_F(PipRmiTest, ToolbarButtonsEmitRefreshAndOutdatedRequests) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_refresh"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("refresh_requested"_key, since));
  fire(id_at(root_ + ".vbox.toolbar.btn_outdated"), "clicked"_key);
  EXPECT_TRUE(srv_->events->wait_for("outdated_requested"_key, since));
}

TEST_F(PipRmiTest, PackageFiltersToggleRowVisibility) {
  call("update_packages"_key, three_packages());
  const std::string table = root_ + ".vbox.table";
  auto visible = [&] { return std::tuple{row_visible(table, 0), row_visible(table, 1), row_visible(table, 2)}; };
  auto state = [&](int32_t v) {
    dynamic p;
    p["value"_key] = v;
    fire(id_at(root_ + ".vbox.toolbar.state"), "changed"_key, std::move(p));
  };

  state(1); // Outdated
  EXPECT_EQ(visible(), (std::tuple{true, false, false}));
  state(2); // Editable
  EXPECT_EQ(visible(), (std::tuple{false, false, true}));
  state(0);
  fire(id_at(root_ + ".vbox.toolbar.filter"), "changed"_key, value_payload("SI"));
  EXPECT_EQ(visible(), (std::tuple{false, true, false}));

  // The filter survives a refresh.
  call("update_packages"_key, three_packages());
  EXPECT_EQ(visible(), (std::tuple{false, true, false}));
}

TEST_F(PipRmiTest, InstallButtonNeedsASpecThenEmitsItWithTheOptions) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.install_bar.btn_install"), "clicked"_key);
  EXPECT_NE(PIP_TEXT_AT(root_ + ".vbox.status").find("Enter a package"), std::string::npos);
  EXPECT_FALSE(srv_->events->wait_for("install_requested"_key, since, nullptr, std::chrono::milliseconds{100}));

  fire(id_at(root_ + ".vbox.install_bar.spec"), "changed"_key, value_payload("requests==2.31.0"));
  fire(id_at(root_ + ".vbox.opts_bar.upgrade"), "changed"_key, bool_payload(true));
  fire(id_at(root_ + ".vbox.opts_bar.pre"), "changed"_key, bool_payload(true));
  fire(id_at(root_ + ".vbox.install_bar.btn_install"), "clicked"_key);
  auto ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("spec"_key), "requests==2.31.0");
  EXPECT_TRUE(ev->as<bool>("upgrade"_key));
  EXPECT_FALSE(ev->as<bool>("user"_key));
  EXPECT_TRUE(ev->as<bool>("pre"_key));
  EXPECT_EQ(srv_->last_session->ui_objects.at(root_ + ".vbox.install_bar.spec")->as<std::string>("value"_key), "");
  EXPECT_NE(PIP_TEXT_AT(root_ + ".vbox.status").find("Running pip install requests==2.31.0"), std::string::npos);
}

TEST_F(PipRmiTest, RequirementsButtonNeedsAPathThenEmitsIt) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.req_bar.btn_requirements"), "clicked"_key);
  EXPECT_NE(PIP_TEXT_AT(root_ + ".vbox.status").find("requirements file"), std::string::npos);

  fire(id_at(root_ + ".vbox.req_bar.req_path"), "changed"_key, value_payload("/work/requirements.txt"));
  fire(id_at(root_ + ".vbox.opts_bar.user"), "changed"_key, bool_payload(true));
  fire(id_at(root_ + ".vbox.req_bar.btn_requirements"), "clicked"_key);
  auto ev = srv_->events->wait_for("requirements_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("path"_key), "/work/requirements.txt");
  EXPECT_TRUE(ev->as<bool>("user"_key));
  EXPECT_FALSE(ev->as<bool>("upgrade"_key));
}

TEST_F(PipRmiTest, UpgradeMenuActionFiresWithoutConfirmation) {
  call("update_packages"_key, three_packages());
  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Upgrade"), "clicked"_key);
  auto ev = srv_->events->wait_for("package_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "requests");
  EXPECT_EQ(ev->as<std::string>("action"_key), "upgrade");
  EXPECT_TRUE(find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_").empty());
}

TEST_F(PipRmiTest, UninstallConfirmsThenEmits) {
  call("update_packages"_key, three_packages());

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 1, "Uninstall..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());
  auto msg = PIP_TEXT_AT(cr + ".body.message");
  EXPECT_NE(msg.find("'six'"), std::string::npos);
  EXPECT_NE(msg.find("1.17.0"), std::string::npos);

  fire_at(cr, id_at(cr + ".buttons.btn0"), "clicked"_key);
  auto ev = srv_->events->wait_for("package_action_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "six");
  EXPECT_EQ(ev->as<std::string>("action"_key), "uninstall");
}

TEST_F(PipRmiTest, UninstallNoCancelsWithoutEmitting) {
  call("update_packages"_key, three_packages());
  fire(menu_id_in(root_ + ".vbox.table", 1, "Uninstall..."), "clicked"_key);
  std::string cr = find_root_with_prefix(srv_->last_session->ui_objects, "__message_box_");
  ASSERT_FALSE(cr.empty());

  size_t since = srv_->events->mark();
  fire_at(cr, id_at(cr + ".buttons.btn1"), "clicked"_key);
  EXPECT_FALSE(
      srv_->events->wait_for("package_action_requested"_key, since, nullptr, std::chrono::milliseconds{100}));
}

TEST_F(PipRmiTest, CommandResultRoutesFailureToScopedWindowStatus) {
  dynamic args;
  args["command"_key] = std::string{"index versions nope"};
  args["scope"_key] = std::string{"versions"};
  args["ok"_key] = false;
  args["output"_key] = std::string{"ERROR: No matching distribution found for nope"};
  call("command_result"_key, std::move(args));
  auto st = PIP_TEXT_AT(root_ + "_versions.vbox.status");
  EXPECT_NE(st.find("failed"), std::string::npos);
  EXPECT_NE(st.find("No matching distribution"), std::string::npos);
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "");

  dynamic ok;
  ok["command"_key] = std::string{"uninstall six"};
  ok["ok"_key] = true;
  call("command_result"_key, std::move(ok));
  EXPECT_EQ(PIP_TEXT_AT(root_ + ".vbox.status"), "uninstall six: OK");
}

// ── Versions ────────────────────────────────────────────────────────────────

TEST_F(PipRmiTest, VersionsMenuActionEmitsAndRowsMarkInstalledAndLatest) {
  call("update_packages"_key, three_packages());
  const std::string table = root_ + "_versions.vbox.table";
  auto note = [&](size_t row) {
    return nth_child(row_in(table, row), 1)->as<std::string>("text"_key);
  };

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Versions"), "clicked"_key);
  auto ev = srv_->events->wait_for("versions_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "requests");
  EXPECT_FALSE(ev->as<bool>("pre"_key));
  EXPECT_EQ(PIP_TEXT_AT(root_ + "_versions.vbox.toolbar.target"), "versions: requests");

  // A response for a package the window no longer shows is dropped.
  call("update_versions"_key, versions_of("six", {"1.17.0"}, "1.17.0"));
  EXPECT_EQ(row_count(table), 0u);

  call("update_versions"_key, versions_of("requests", {"2.34.2", "2.32.0", "2.31.0"}, "2.34.2"));
  ASSERT_EQ(row_count(table), 3u);
  EXPECT_EQ(note(0), "latest");
  EXPECT_EQ(note(1), "");
  EXPECT_EQ(note(2), "installed");
  EXPECT_EQ(PIP_TEXT_AT(root_ + "_versions.vbox.status"), "3 versions, installed: 2.31.0");

  // The mark follows the next packages snapshot (after an upgrade, say).
  auto upgraded = make_list_args("packages", {{{"name", "Requests"}, {"version", "2.34.2"}}});
  call("update_packages"_key, std::move(upgraded));
  ASSERT_EQ(row_count(table), 3u);
  EXPECT_EQ(note(0), "installed, latest");
  EXPECT_EQ(note(2), "");
}

TEST_F(PipRmiTest, InstallThisVersionEmitsAPinnedSpec) {
  call("update_packages"_key, three_packages());
  fire(menu_id_in(root_ + ".vbox.table", 0, "Versions"), "clicked"_key);
  call("update_versions"_key, versions_of("requests", {"2.34.2", "2.31.0"}, "2.34.2"));
  fire(id_at(root_ + ".vbox.opts_bar.upgrade"), "changed"_key, bool_payload(true));

  size_t since = srv_->events->mark();
  fire_at(
      root_ + "_versions", menu_id_in(root_ + "_versions.vbox.table", 0, "Install this version"), "clicked"_key);
  auto ev = srv_->events->wait_for("install_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("spec"_key), "requests==2.34.2");
  EXPECT_FALSE(ev->as<bool>("upgrade"_key)) << "a pinned version ignores the Upgrade checkbox";
}

TEST_F(PipRmiTest, VersionsButtonUsesThePackageNameOfTheTypedSpec) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.install_bar.btn_versions"), "clicked"_key);
  EXPECT_NE(PIP_TEXT_AT(root_ + ".vbox.status").find("Enter a package name"), std::string::npos);

  fire(id_at(root_ + ".vbox.install_bar.spec"), "changed"_key, value_payload(" requests[socks]>=2.0"));
  fire(id_at(root_ + ".vbox.opts_bar.pre"), "changed"_key, bool_payload(true));
  fire(id_at(root_ + ".vbox.install_bar.btn_versions"), "clicked"_key);
  auto ev = srv_->events->wait_for("versions_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("name"_key), "requests");
  EXPECT_TRUE(ev->as<bool>("pre"_key));
}

TEST_F(PipRmiTest, VersionRowsAreCappedWithAHint) {
  fire(id_at(root_ + ".vbox.install_bar.spec"), "changed"_key, value_payload("big"));
  fire(id_at(root_ + ".vbox.install_bar.btn_versions"), "clicked"_key);
  std::vector<std::string> versions;
  for (int i = 0; i < 210; ++i)
    versions.push_back("1." + std::to_string(i));
  call("update_versions"_key, versions_of("big", versions, "1.0"));
  EXPECT_EQ(row_count(root_ + "_versions.vbox.table"), 200u);
  auto status = PIP_TEXT_AT(root_ + "_versions.vbox.status");
  EXPECT_NE(status.find("210 versions, not installed"), std::string::npos) << status;
  EXPECT_NE(status.find("newest 200"), std::string::npos) << status;
}

// ── Details ─────────────────────────────────────────────────────────────────

TEST_F(PipRmiTest, DetailsMenuActionEmitsAndUpdateDetailsFillsEditor) {
  call("update_packages"_key, three_packages());
  const std::string editor = root_ + "_details.vbox.editor";

  size_t since = srv_->events->mark();
  fire(menu_id_in(root_ + ".vbox.table", 0, "Files"), "clicked"_key);
  auto ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "files");
  EXPECT_EQ(ev->as<std::string>("name"_key), "requests");

  auto details = [](const std::string& kind, const std::string& name, const std::string& text) {
    dynamic d;
    d["kind"_key] = kind;
    d["name"_key] = name;
    d["title"_key] = kind + ": " + name;
    d["text"_key] = text;
    return d;
  };
  call("update_details"_key, details("files", "requests", "Name: requests\n"));
  const std::string first = editor_file(editor);
  EXPECT_EQ(first.rfind("private/", 0), 0u) << first;
  EXPECT_EQ(editor_text(editor), "Name: requests\n");

  // A response for a target the window no longer shows is dropped.
  call("update_details"_key, details("show", "requests", "stale"));
  call("update_details"_key, details("files", "six", "stale"));
  EXPECT_EQ(editor_file(editor), first);

  // Closing any window removes the file.
  const auto file = srv_->last_session->resource_dir / first;
  ASSERT_TRUE(std::filesystem::exists(file));
  fire_at(root_ + "_details", id_at(root_ + "_details"), "closed"_key);
  EXPECT_FALSE(std::filesystem::exists(file));
}

TEST_F(PipRmiTest, FreezeAndCheckButtonsRequestEnvironmentWideDetails) {
  size_t since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_freeze"), "clicked"_key);
  auto ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "freeze");
  EXPECT_EQ(ev->as<std::string>("name"_key), "");
  EXPECT_EQ(PIP_TEXT_AT(root_ + "_details.vbox.toolbar.target"), "pip freeze");

  since = srv_->events->mark();
  fire(id_at(root_ + ".vbox.toolbar.btn_check"), "clicked"_key);
  ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "check");

  // The Details window's own Refresh re-requests the open target.
  since = srv_->events->mark();
  fire_at(root_ + "_details", id_at(root_ + "_details.vbox.toolbar.btn_refresh"), "clicked"_key);
  ev = srv_->events->wait_for("details_requested"_key, since);
  ASSERT_TRUE(ev);
  EXPECT_EQ(ev->as<std::string>("kind"_key), "check");
}

// ── Console window (client `pip` subprocess trace) ──────────────────────────

TEST_F(PipRmiTest, AppendCommandLogAddsRowsAndClearConsoleEmptiesThem) {
  auto log_args = [](const std::string& command, int32_t exit_code, bool ok, const std::string& output) {
    dynamic a;
    a["command"_key] = command;
    a["exit_code"_key] = exit_code;
    a["ok"_key] = ok;
    a["output"_key] = output;
    return a;
  };
  const std::string table = root_ + "_console.vbox.table";

  EXPECT_EQ(row_count(table), 0u);
  call("append_command_log"_key, log_args("pip list --format=json", 0, true, ""));
  call("append_command_log"_key, log_args("pip uninstall -y nope", 1, false, "not installed"));
  EXPECT_EQ(row_count(table), 2u);

  auto clear = menu_id_in(table, 0, "Clear Console");
  ASSERT_NE(clear.id, 0u);
  fire_at(root_ + "_console", clear, "clicked"_key);
  EXPECT_EQ(row_count(table), 0u);
}
