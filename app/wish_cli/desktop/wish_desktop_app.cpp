// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.cpp
 * @brief wish CLI desktop mode implementation.
 */
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include "app/wish_cli/env_flags.hpp"
#include "src/bison/bison.hpp"
#include "src/client/app_registry.hpp"
#include "src/client/user_store_rpc.hpp"
#include "src/rmi/shared/constants.hpp"
#include "src/term/terminal.hpp"
#include "src/ui/ui_descriptor.hpp"

#include <gflags/gflags.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

// ── Shared flags (defined in main.cpp for wish-cli, or desktop/standalone_main.cpp
// for the standalone wish-desktop binary) ────────────────────────────────────
DECLARE_string(cmd);
DECLARE_string(verbose);
DECLARE_bool(debugger);
// Shared with wish client's --timeout: both express "per-request timeout to
// the remote peer" (there, the wish server; here, the upstream server).
DECLARE_int32(timeout);

// ── Downstream (server side) flags ─────────────────────────────────────────────
DEFINE_string(downstream_transport, "tcp", "Downstream transport to use: tcp, pipe, tls, or term");
DEFINE_string(downstream_host, "0.0.0.0", "Downstream bind host address (downstream_transport=tcp/tls)");
DEFINE_int32(downstream_port, 7071, "Downstream listen port (downstream_transport=tcp/tls)");
DEFINE_string(downstream_name, "", "Downstream named-pipe / Unix-socket path (downstream_transport=pipe)");
DEFINE_string(downstream_cert_file, "", "Downstream server certificate chain file (downstream_transport=tls)");
DEFINE_string(downstream_cert_pem, "", "Downstream server certificate chain PEM (downstream_transport=tls)");
DEFINE_string(downstream_key_file, "", "Downstream server private key file (downstream_transport=tls)");
DEFINE_string(downstream_key_pem, "", "Downstream server private key PEM (downstream_transport=tls)");
DEFINE_string(downstream_key_password, "", "Passphrase for an encrypted downstream private key (downstream_transport=tls)");
DEFINE_string(downstream_client_auth, "none",
    "Downstream mutual TLS mode: none, optional, or required (downstream_transport=tls)");
DEFINE_string(downstream_ca_file, "",
    "Trust anchor file for verifying downstream client certificates (downstream_transport=tls, "
    "downstream_client_auth!=none)");
DEFINE_string(downstream_ca_pem, "",
    "Trust anchor PEM for verifying downstream client certificates (downstream_transport=tls, "
    "downstream_client_auth!=none)");

// ── Upstream (client side) flags ─────────────────────────────────────────────
DEFINE_string(upstream_transport, "term", "Upstream transport to use: tcp, pipe, tls, or term");
DEFINE_string(upstream_host, "127.0.0.1", "Upstream host address (upstream_transport=tcp/tls)");
DEFINE_int32(upstream_port, 7070, "Upstream port (upstream_transport=tcp/tls)");
DEFINE_string(upstream_name, "", "Upstream named-pipe / Unix-socket path (upstream_transport=pipe)");
DEFINE_string(upstream_ca_file, "", "Trust anchor file for verifying the upstream server's cert (upstream_transport=tls)");
DEFINE_string(upstream_ca_pem, "", "Trust anchor PEM for verifying the upstream server's cert (upstream_transport=tls)");
DEFINE_string(upstream_server_name, "", "SNI / hostname-verification target (upstream_transport=tls, default: --upstream_host)");
DEFINE_bool(upstream_insecure_skip_verify, false,
    "Skip upstream server certificate verification -- unsafe, dev/test only (upstream_transport=tls)");
DEFINE_string(upstream_cert_file, "", "Upstream client certificate file, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_cert_pem, "", "Upstream client certificate PEM, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_file, "", "Upstream client private key file, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_pem, "", "Upstream client private key PEM, for mutual TLS (upstream_transport=tls)");
DEFINE_string(upstream_key_password, "", "Passphrase for an encrypted upstream private key (upstream_transport=tls)");
DEFINE_string(
    upstream_username,
    "",
    "Identity the desktop connects upstream as; its user store keeps the desktop's pinned apps, desktop "
    "icons and theme (default: the OS user name)");

// ── Desktop shell flags ──────────────────────────────────────────────────────
DEFINE_string(wallpaper, "", "Image file shown as the desktop background (default: built-in gradient)");

namespace bdg::wish {

using namespace bison;

namespace {

/**
 * @brief Sets an environment variable so a subsequently-spawned child
 *        process inherits it (e.g. via `forkpty()`+`execl()` on POSIX, or
 *        `CreateProcess()` on Windows, both of which inherit the calling
 *        process's environment by default).
 */
void set_env_var(const char* name, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

// Template name used to register/instantiate the desktop shell; namespaced
// with a leading "__" like wish's other internal protocol identifiers to
// avoid colliding with a downstream client's own template names.
constexpr const char* kChromeTemplateName = "__wish_desktop_chrome";

// User store entry holding desktop_settings.
constexpr const char* kSettingsEntry = "wish.desktop";

// Where an uploaded wallpaper lives in the upstream sandbox (the desktop's
// private app directory, see src/auth/DESIGN.md).
constexpr const char* kWallpaperDir = "private/apps/wish.desktop";

// Apps pinned to the taskbar and shown on the desktop until the user
// changes them -- the first of these that are installed.
const std::vector<std::string> kDefaultPinned{"nano", "mc", "top"};

std::string os_user_name() {
  for (const char* var : {"USER", "USERNAME", "LOGNAME"}) {
    if (const char* v = std::getenv(var); v && *v)
      return v;
  }
  return {};
}

/// A wildcard bind address isn't connectable; children dial loopback.
std::string connectable_host(const std::string& bind_host) {
  if (bind_host.empty() || bind_host == "0.0.0.0")
    return "127.0.0.1";
  if (bind_host == "::" || bind_host == "[::]")
    return "::1";
  return bind_host;
}

std::vector<std::string> string_list(const dynamic& d, key_t k) {
  std::vector<std::string> out;
  const field* f = d.findField(k);
  if (!f || !f->is<dynamic_ptr>() || !f->as<dynamic_ptr>())
    return out;
  f->as<dynamic_ptr>()->forEach([&](key_t, const field& v) {
    if (v.is<std::string>())
      out.push_back(v.as<std::string>());
  });
  return out;
}

dynamic_ptr string_list_value(const std::vector<std::string>& v) {
  auto d = std::make_shared<dynamic>();
  for (size_t i = 0; i < v.size(); ++i)
    (*d)[key_t{static_cast<uint32_t>(i)}] = v[i];
  return dynamic_ptr{d};
}

/// Whether @p wanted names @p info, by short or qualified name (default pins
/// are short names).
bool name_matches(const app_info& info, const std::string& wanted) {
  return info.name == wanted || qualified_app_name(info) == wanted;
}

} // namespace

// ── wish_desktop — desktop shell ────────────────────────────────────────────

wish_desktop::~wish_desktop() {
  stop_worker();
}

void wish_desktop::set_client_command(std::vector<std::string> command) {
  client_command_ = std::move(command);
}

void wish_desktop::set_launch_env(std::vector<std::pair<std::string, std::string>> env) {
  launch_env_ = std::move(env);
}

void wish_desktop::set_log_dir(std::filesystem::path dir) {
  log_dir_ = std::move(dir);
}

void wish_desktop::set_launcher(process_launcher_iface* launcher) {
  launcher_ = launcher;
}

void wish_desktop::set_wallpaper(std::filesystem::path file) {
  wallpaper_ = std::move(file);
}

void wish_desktop::set_user(std::string user) {
  user_ = std::move(user);
}

desktop_settings wish_desktop::settings() const {
  return state_.rlock()->settings;
}

// ── Worker thread ──

void wish_desktop::post(std::function<void()> task) {
  {
    auto w = worker_.wlock();
    if (w->stop)
      return;
    w->tasks.push_back(std::move(task));
    if (!worker_thread_.joinable())
      worker_thread_ = std::thread([this] { worker_loop(); });
  }
  worker_.notify_all();
}

void wish_desktop::worker_loop() {
  for (;;) {
    std::function<void()> task;
    worker_.wait([](const worker_state& ws) { return ws.stop || !ws.tasks.empty(); });
    {
      auto w = worker_.wlock();
      // On stop, pending work is dropped: the upstream is going away.
      if (w->stop)
        return;
      if (w->tasks.empty())
        continue;
      task = std::move(w->tasks.front());
      w->tasks.pop_front();
      ++w->busy;
    }
    try {
      task();
    } catch (const std::exception& ex) {
      std::cerr << "[desktop] " << ex.what() << '\n';
    } catch (...) {
    }
    worker_.wlock()->busy -= 1;
    worker_.notify_all();
  }
}

void wish_desktop::stop_worker() {
  worker_.wlock()->stop = true;
  worker_.notify_all();
  if (worker_thread_.joinable())
    worker_thread_.join();
}

void wish_desktop::flush() {
  worker_.wait([](const worker_state& ws) { return ws.stop || (ws.tasks.empty() && ws.busy == 0); });
}

// ── Shell construction ──

void wish_desktop::build_chrome() {
  if (chrome_built_.exchange(true))
    return;

  try {
    // Saved settings (identified upstream sessions only).
    try {
      user_store_proxy_ = upstream().instantiate("wish"_key, "__WishUserStore"_key).get();
    } catch (...) {
      user_store_proxy_.reset();
    }
    desktop_settings saved;
    bool have_saved = false;
    if (user_store_proxy_) {
      try {
        if (auto entry = detail::user_store_get_sync(*user_store_proxy_, kSettingsEntry)) {
          saved.pinned = string_list(*entry, "pinned"_key);
          saved.on_desktop = string_list(*entry, "desktop"_key);
          saved.theme = entry->get_as<std::string>("theme"_key, std::string{});
          have_saved = true;
        }
      } catch (const std::exception& ex) {
        std::cerr << "[desktop] cannot read saved settings: " << ex.what() << '\n';
      }
    }

    // The registered apps, sorted by collection then name.
    std::vector<const app_info*> apps;
    for (const auto& [name, info] : registered_apps())
      apps.push_back(&info);
    std::stable_sort(apps.begin(), apps.end(), [](const app_info* a, const app_info* b) {
      return std::tie(a->collection, a->name) < std::tie(b->collection, b->name);
    });
    if (!have_saved) {
      for (const auto& wanted : kDefaultPinned) {
        for (const app_info* a : apps) {
          if (name_matches(*a, wanted)) {
            saved.pinned.push_back(qualified_app_name(*a));
            saved.on_desktop.push_back(qualified_app_name(*a));
            break;
          }
        }
      }
    }
    state_.wlock()->settings = saved;

    // Theme.
    try {
      style_proxy_ = upstream().instantiate("wish"_key, "__WishStyle"_key).get();
    } catch (...) {
      style_proxy_.reset();
    }
    if (!saved.theme.empty())
      apply_theme(saved.theme);

    // Wallpaper: uploaded into the upstream sandbox, then referenced by path.
    std::string wallpaper_src;
    if (!wallpaper_.empty()) {
      std::ifstream in(wallpaper_, std::ios::binary);
      if (!in) {
        std::cerr << "[desktop] cannot read wallpaper " << wallpaper_.string() << '\n';
      } else {
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string ext = wallpaper_.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        wallpaper_src = std::string(kWallpaperDir) + "/wallpaper" + ext;
        try {
          auto fs = upstream().instantiate("wish"_key, "__WishFileSystem"_key).get();
          dynamic args;
          args["name"_key] = wallpaper_src;
          args["data"_key] = std::move(bytes);
          args["first"_key] = true;
          args["eof"_key] = true;
          fs.call("upload_chunk"_key, std::move(args)).get();
        } catch (const std::exception& ex) {
          std::cerr << "[desktop] wallpaper upload failed: " << ex.what() << '\n';
          wallpaper_src.clear();
        }
      }
    }

    // The DesktopShell descriptor, one DesktopApp child per registered app.
    auto contains = [](const std::vector<std::string>& v, const std::string& x) {
      return std::find(v.begin(), v.end(), x) != v.end();
    };
    nlohmann::ordered_json root;
    root["type"] = "DesktopShell";
    root["user"] = user_;
    root["wallpaper"] = wallpaper_src;
    nlohmann::ordered_json children = nlohmann::ordered_json::object();
    std::map<std::string, std::string> path_to_app;
    for (size_t i = 0; i < apps.size(); ++i) {
      const app_info& a = *apps[i];
      std::string qualified = qualified_app_name(a);
      std::string child = "app" + std::to_string(i);
      path_to_app[child] = qualified;
      nlohmann::ordered_json c;
      c["type"] = "DesktopApp";
      c["app"] = qualified;
      c["label"] = a.name;
      c["description"] = a.description;
      c["collection"] = a.collection;
      c["icon"] = a.icon;
      c["pinned"] = contains(saved.pinned, qualified);
      c["on_desktop"] = contains(saved.on_desktop, qualified);
      children[child] = std::move(c);
    }
    root["children"] = std::move(children);

    template_proxy_ = upstream().instantiate("wish"_key, "__WishTemplate"_key).get();
    auto& tmpl = *template_proxy_;

    dynamic reg_args;
    reg_args["name"_key] = key_t{kChromeTemplateName};
    reg_args["descriptor"_key] = dynamic_ptr{import_descriptor_json(root.dump())};
    tmpl.call("register"_key, std::move(reg_args)).get();

    dynamic inst_args;
    inst_args["name"_key] = key_t{kChromeTemplateName};
    auto result = tmpl.call("instantiate"_key, std::move(inst_args)).get();

    key_t shell_id{0u};
    std::map<std::string, key_t> app_nodes;
    result.forEach([&](key_t, const field& v) {
      if (!v.is<dynamic_ptr>())
        return;
      const auto& entry = v.as<dynamic_ptr>();
      if (!entry)
        return;
      const auto* name_field = entry->findField("name"_key);
      const auto* id_field = entry->findField("id"_key);
      if (!name_field || !id_field || !name_field->is<std::string>() || !id_field->is<key_t>())
        return;
      const std::string& path = name_field->as<std::string>();
      if (path.empty())
        shell_id = id_field->as<key_t>();
      else if (auto it = path_to_app.find(path); it != path_to_app.end())
        app_nodes[it->second] = id_field->as<key_t>();
    });

    {
      auto st = state_.wlock();
      st->shell_id = shell_id;
      st->app_nodes = std::move(app_nodes);
    }

    if (shell_id.id != 0u) {
      shell_proxy_ = upstream().make_proxy(shell_id);
      for (const char* ev : {"launch", "pin", "desktop_icon", "theme", "quit"}) {
        key_t name{ev};
        shell_proxy_->onEvent(name, [this, name](dynamic payload) { on_shell_event(name, payload); });
      }
    }
    // Clients that connected while the shell was being built.
    post([this] { push_running_state(); });
  } catch (const std::exception& ex) {
    std::cerr << "[desktop] chrome build error: " << ex.what() << '\n';
  }
}

// ── Shell events ──

void wish_desktop::on_shell_event(key_t event, const dynamic& payload) {
  const std::string app = payload.get_as<std::string>("app"_key, std::string{});

  if (event == key_t{"quit"}) {
    request_quit();
  } else if (event == key_t{"launch"}) {
    post([this, app] { launch_app(app); });
  } else if (event == key_t{"pin"} || event == key_t{"desktop_icon"}) {
    const bool pin = event == key_t{"pin"};
    const bool value = payload.get_as<bool>(pin ? "pinned"_key : "on_desktop"_key, false);
    post([this, app, pin, value] {
      key_t node{0u};
      {
        auto st = state_.wlock();
        auto& list = pin ? st->settings.pinned : st->settings.on_desktop;
        list.erase(std::remove(list.begin(), list.end(), app), list.end());
        if (value)
          list.push_back(app);
        if (auto it = st->app_nodes.find(app); it != st->app_nodes.end())
          node = it->second;
      }
      if (node.id != 0u) {
        dynamic fields;
        fields[pin ? "pinned"_key : "on_desktop"_key] = value;
        upstream().make_proxy(node).set(std::move(fields)).get();
      }
      save_settings();
    });
  } else if (event == key_t{"theme"}) {
    const std::string preset = payload.get_as<std::string>("preset"_key, std::string{});
    post([this, preset] {
      state_.wlock()->settings.theme = preset;
      apply_theme(preset);
      save_settings();
    });
  }
}

void wish_desktop::apply_theme(const std::string& preset) {
  if (!style_proxy_ || preset.empty())
    return;
  dynamic args;
  args["name"_key] = preset;
  style_proxy_->call("preset"_key, std::move(args)).get();
}

void wish_desktop::save_settings() {
  if (!user_store_proxy_)
    return;
  desktop_settings s = settings();
  dynamic entry;
  entry["pinned"_key] = string_list_value(s.pinned);
  entry["desktop"_key] = string_list_value(s.on_desktop);
  entry["theme"_key] = s.theme;
  detail::user_store_set_sync(*user_store_proxy_, kSettingsEntry, std::move(entry));
}

bool wish_desktop::launch_app(const std::string& app) {
  if (!launcher_ || client_command_.empty()) {
    std::cerr << "[desktop] cannot launch " << app << ": no launcher configured\n";
    return false;
  }
  launch_request req;
  req.argv = client_command_;
  req.argv.push_back("--run=" + app);
  req.env = launch_env_;
  if (!log_dir_.empty()) {
    std::string file = app;
    std::replace(file.begin(), file.end(), '/', '.');
    req.log_path = log_dir_ / (file + "_" + std::to_string(++launch_count_) + ".log");
  }
  std::string error;
  if (!launcher_->launch(req, error)) {
    std::cerr << "[desktop] cannot launch " << app << ": " << error << '\n';
    return false;
  }
  return true;
}

// ── Downstream clients ──

bool wish_desktop::try_handle_request(
    rmi::context& ctx,
    const rmi::shared::envelope& env,
    rmi::transport::server_connection_iface& conn) {
  if (env.op == rmi::shared::constants::OP_CONNECT) {
    std::string app = env.payload.get_as<std::string>("app"_key, std::string{});
    {
      auto st = state_.wlock();
      st->sessions.insert(ctx.session_id.id);
      if (!app.empty()) {
        st->session_apps[ctx.session_id.id] = app;
        st->app_meta[app] = {
            env.payload.get_as<std::string>("app_label"_key, std::string{}),
            env.payload.get_as<std::string>("app_icon"_key, std::string{})};
      }
    }
    post([this] { push_running_state(); });
  }
  return bridge::try_handle_request(ctx, env, conn);
}

void wish_desktop::on_client_disconnected(rmi::context& ctx) {
  {
    auto st = state_.wlock();
    st->sessions.erase(ctx.session_id.id);
    st->session_apps.erase(ctx.session_id.id);
  }
  post([this] { push_running_state(); });
}

namespace {

/// The `app_nodes` entry @p app runs as: its exact name, or a registered
/// app whose short name it is (e.g. a client run as "--run=nano").
template <typename Map>
auto find_app_node(Map& nodes, const std::string& app) {
  auto it = nodes.find(app);
  if (it != nodes.end())
    return it;
  return std::find_if(nodes.begin(), nodes.end(), [&](const auto& e) {
    auto slash = e.first.rfind('/');
    return slash != std::string::npos && e.first.substr(slash + 1) == app;
  });
}

} // namespace

void wish_desktop::add_unknown_apps() {
  if (!template_proxy_)
    return;
  std::vector<std::pair<std::string, std::pair<std::string, std::string>>> unknown;
  {
    auto st = state_.rlock();
    for (const auto& [session, app] : st->session_apps) {
      if (find_app_node(st->app_nodes, app) != st->app_nodes.end())
        continue;
      auto meta = st->app_meta.find(app);
      unknown.emplace_back(app, meta != st->app_meta.end() ? meta->second : std::pair<std::string, std::string>{});
    }
  }
  for (const auto& [app, meta] : unknown) {
    {
      auto st = state_.rlock();
      if (find_app_node(st->app_nodes, app) != st->app_nodes.end())
        continue; // two sessions of the same new app
    }
    std::string label = meta.first;
    if (label.empty())
      label = app.substr(app.rfind('/') == std::string::npos ? 0 : app.rfind('/') + 1);
    nlohmann::ordered_json d;
    d["type"] = "DesktopApp";
    d["app"] = app;
    d["label"] = label;
    d["icon"] = meta.second;
    d["listed"] = false;
    // A one-off template per app: instantiating it adds a top-level
    // DesktopApp next to the shell, which the server lists with the shell's
    // own children (see render_desktop_shell()).
    std::string name = std::string(kChromeTemplateName) + "_app" + std::to_string(state_.wlock()->extra_apps++);
    dynamic reg_args;
    reg_args["name"_key] = key_t{name.c_str()};
    reg_args["descriptor"_key] = dynamic_ptr{import_descriptor_json(d.dump())};
    template_proxy_->call("register"_key, std::move(reg_args)).get();
    dynamic inst_args;
    inst_args["name"_key] = key_t{name.c_str()};
    auto result = template_proxy_->call("instantiate"_key, std::move(inst_args)).get();
    key_t node{0u};
    result.forEach([&](key_t, const field& v) {
      if (!v.is<dynamic_ptr>() || !v.as<dynamic_ptr>())
        return;
      const auto& entry = v.as<dynamic_ptr>();
      const auto* name_field = entry->findField("name"_key);
      const auto* id_field = entry->findField("id"_key);
      if (name_field && id_field && name_field->is<std::string>() && id_field->is<key_t>() &&
          name_field->as<std::string>().empty())
        node = id_field->as<key_t>();
    });
    if (node.id != 0u)
      state_.wlock()->app_nodes[app] = node;
  }
}

void wish_desktop::push_running_state() {
  add_unknown_apps();
  key_t shell_id{0u};
  int32_t clients = 0;
  std::map<key_t, std::vector<int32_t>> by_node;
  {
    auto st = state_.rlock();
    shell_id = st->shell_id;
    clients = static_cast<int32_t>(st->sessions.size());
    for (const auto& [app, node] : st->app_nodes)
      by_node[node]; // every app, so one that stopped is cleared too
    for (const auto& [session, app] : st->session_apps) {
      auto it = find_app_node(st->app_nodes, app);
      if (it != st->app_nodes.end())
        by_node[it->second].push_back(static_cast<int32_t>(session));
    }
  }
  if (shell_id.id == 0u)
    return;
  dynamic shell_fields;
  shell_fields["clients"_key] = clients;
  upstream().make_proxy(shell_id).set(std::move(shell_fields)).get();
  for (auto& [node, sessions] : by_node) {
    dynamic fields;
    fields["sessions"_key] = std::move(sessions);
    upstream().make_proxy(node).set(std::move(fields)).get();
  }
}

void wish_desktop::request_quit() {
  {
    std::lock_guard<std::mutex> lk(quit_mtx_);
    quit_requested_ = true;
  }
  quit_cv_.notify_all();
}

void wish_desktop::wait_for_quit() {
  std::unique_lock<std::mutex> lk(quit_mtx_);
  quit_cv_.wait(lk, [this] { return quit_requested_; });
}

bool wish_desktop::wait_for_quit_for(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(quit_mtx_);
  return quit_cv_.wait_for(lk, timeout, [this] { return quit_requested_; });
}

// ── wish_desktop_app ─────────────────────────────────────────────────────────

int wish_desktop_app::run(int argc, char** argv) {
  apply_env_flag_defaults();
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  set_env_var("WISH_TRANSPORT", FLAGS_downstream_transport);
  set_env_var("WISH_HOST", FLAGS_downstream_host);
  set_env_var("WISH_PORT", std::to_string(FLAGS_downstream_port));
  set_env_var("WISH_NAME", FLAGS_downstream_name);

  return bison::app::bridge_app::run(argc, argv);
}

std::string wish_desktop_app::bridge_description() const {
  return "wish desktop.\n"
         "Multiplexes downstream clients into one upstream wish server, "
         "rendering a desktop (wallpaper, taskbar, Start menu) whose Start "
         "menu launches the registered apps as downstream clients.";
}

std::unique_ptr<bison::rmi::bridge> wish_desktop_app::make_bridge(
    bison::rmi::transport::server_transport_iface& downstream_transport,
    std::unique_ptr<bison::rmi::transport::client_transport_iface> upstream_transport,
    bison::dynamic upstream_params) {
  auto br =
      std::make_unique<wish_desktop>(downstream_transport, std::move(upstream_transport), std::move(upstream_params));

  if (FLAGS_downstream_transport == "term") {
    std::cerr << "[desktop] --downstream_transport=term: apps cannot be launched from the Start menu\n";
  } else {
    if (!launcher_)
      launcher_ = std::make_unique<process_launcher>();
    br->set_launcher(launcher_.get());
    br->set_client_command(client_command_);
    br->set_launch_env({
        {"WISH_TRANSPORT", FLAGS_downstream_transport},
        {"WISH_HOST", connectable_host(FLAGS_downstream_host)},
        {"WISH_PORT", std::to_string(FLAGS_downstream_port)},
        {"WISH_NAME", FLAGS_downstream_name},
    });
    br->set_log_dir(std::filesystem::path("wish_logs") / "desktop");
  }
  if (!FLAGS_wallpaper.empty())
    br->set_wallpaper(FLAGS_wallpaper);
  br->set_user(FLAGS_upstream_username.empty() ? os_user_name() : FLAGS_upstream_username);

  desktop_ = br.get();
  return br;
}

void wish_desktop_app::on_listening() const {
  bison::app::bridge_app::on_listening();
  if (desktop_)
    desktop_->build_chrome();
}

void wish_desktop_app::on_upstream_connect_params(bison::dynamic& params) const {
  bison::app::bridge_app::on_upstream_connect_params(params);
  std::string user = FLAGS_upstream_username.empty() ? os_user_name() : FLAGS_upstream_username;
  if (!user.empty())
    params["username"_key] = user;
}

void wish_desktop_app::wait_for_shutdown() {
  if (!desktop_) {
    bison::app::bridge_app::wait_for_shutdown();
    return;
  }

  using namespace std::chrono_literals;
  while (!desktop_->wait_for_quit_for(100ms)) {
    if (active_term_ && active_term_->has_exited())
      break;
  }
  // Apps launched from the desktop go down with it.
  if (launcher_)
    launcher_->terminate_all();
}

} // namespace bdg::wish
