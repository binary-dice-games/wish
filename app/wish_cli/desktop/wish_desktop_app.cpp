// MIT License © 2025 Binary Dice Games
/**
 * @file wish_desktop_app.cpp
 * @brief wish CLI desktop mode implementation.
 */
#include "app/wish_cli/desktop/wish_desktop_app.hpp"

#include "app/wish_cli/env_flags.hpp"
#include "src/bison/bison.hpp"
#include "src/client/app_registry.hpp"
#include "src/client/desktop_endpoint.hpp"
#include "src/client/style_rpc.hpp"
#include "src/client/user_store_rpc.hpp"
#include "src/context/os_user.hpp"
#include "src/context/wish_home.hpp"
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

// Defined in config_flags.cpp for wish-cli; wish-desktop has no server flags.
#ifdef WISH_CLI_BUILD
DECLARE_string(wish_home);
#else
DEFINE_string(wish_home, "", "Root of the files wish keeps for its users (see src/context/DESIGN.md); empty: ~/.wish");
#endif

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
DEFINE_string(
    config,
    "",
    "Desktop configuration file (YAML or JSON): the Start menu and other desktop settings; "
    "created with the defaults if missing (default: <wish_home>/users/<user>/desktop/desktop.yaml)");
DEFINE_bool(
    with_server,
    false,
    "Run the wish server in this process, connected through an in-memory transport, instead of "
    "connecting to a separate one (the --upstream_* transport flags are ignored; the wish server "
    "flags such as --renderer apply). Combined wish binary only.");

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

/// The default config file: in @p user's profile, or -- for a name that
/// can't be a profile (see normalize_identity()) -- directly in the home.
std::filesystem::path desktop_config_path(const wish_home& home, const std::string& user) {
  auto file = home.user_desktop_config(user);
  return file.empty() ? home.root() / "desktop.yaml" : file;
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

/// The home directory, for the config's `{home}`.
std::string home_dir() {
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  return home ? home : "";
}

/// A config `args:` map (JSON) as a bison object of scalars.
dynamic json_to_dynamic(const nlohmann::ordered_json& j) {
  dynamic d;
  if (!j.is_object())
    return d;
  for (const auto& [k, v] : j.items()) {
    key_t key{k.c_str()};
    if (v.is_string())
      d[key] = v.get<std::string>();
    else if (v.is_boolean())
      d[key] = v.get<bool>();
    else if (v.is_number_integer())
      d[key] = static_cast<int32_t>(v.get<long long>());
    else if (v.is_number())
      d[key] = v.get<float>();
  }
  return d;
}

/// Whether a client that connected as @p app runs @p entry_app: the same
/// name, or the short name of it (a client run as "--run=nano").
bool app_matches(const std::string& entry_app, const std::string& app) {
  if (entry_app.empty() || app.empty())
    return false;
  if (entry_app == app)
    return true;
  auto slash = entry_app.rfind('/');
  return slash != std::string::npos && app.find('/') == std::string::npos && entry_app.substr(slash + 1) == app;
}

/// The file's modification time and size, to notice edits; nullopt when it
/// doesn't exist (an editor saving through a rename removes it briefly).
std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> file_stamp(
    const std::filesystem::path& file) {
  std::error_code ec;
  auto t = std::filesystem::last_write_time(file, ec);
  if (ec)
    return std::nullopt;
  auto size = std::filesystem::file_size(file, ec);
  if (ec)
    return std::nullopt;
  return std::make_pair(t, size);
}

} // namespace

// ── wish_desktop — desktop shell ────────────────────────────────────────────

wish_desktop::~wish_desktop() {
  stop_tasks();
}

void wish_desktop::stop_tasks() {
  stop_watcher();
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

void wish_desktop::set_config_file(std::filesystem::path file) {
  config_file_ = std::move(file);
}

void wish_desktop::set_config_poll_interval(std::chrono::milliseconds interval) {
  config_poll_ = interval;
}

desktop_settings wish_desktop::settings() const {
  return state_.rlock()->settings;
}

desktop::start_menu wish_desktop::start_menu() const {
  return state_.rlock()->menu;
}

std::string wish_desktop::config_error() const {
  return state_.rlock()->config_error;
}

desktop::placeholders wish_desktop::placeholders() const {
  return {client_command_, config_file_.string(), home_dir()};
}

// ── Worker and watcher threads ──

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

void wish_desktop::stop_watcher() {
  *watcher_stop_.wlock() = true;
  watcher_stop_.notify_all();
  if (watcher_thread_.joinable())
    watcher_thread_.join();
}

void wish_desktop::flush() {
  worker_.wait([](const worker_state& ws) { return ws.stop || (ws.tasks.empty() && ws.busy == 0); });
}

// ── Shell construction ──

void wish_desktop::build_chrome() {
  if (chrome_built_.exchange(true))
    return;

  try {
    // The themes the server has: the settings' choices and toggle_theme's
    // cycle.
    try {
      style_proxy_ = upstream().instantiate("wish"_key, "__WishStyle"_key).get();
      *themes_.wlock() = detail::list_themes_sync(*style_proxy_);
    } catch (...) {
      style_proxy_.reset();
    }
    // Saved settings (identified upstream sessions only).
    try {
      user_store_proxy_ = upstream().instantiate("wish"_key, "__WishUserStore"_key).get();
    } catch (...) {
      user_store_proxy_.reset();
    }
    // Settings metadata for the settings tool (bdg/desktop/conf).
    if (user_store_proxy_) {
      try {
        settings::setting_info theme;
        theme.name = std::string(kSettingsEntry) + ".theme";
        theme.label = "Desktop theme";
        theme.description = "Theme of the desktop and the apps on it; the tray's theme button switches it too.";
        theme.category = "desktop";
        theme.type = settings::setting_type::choice;
        for (const auto& t : *themes_.rlock())
          theme.options.push_back({t.name, t.label});
        if (theme.options.empty()) // an older server: the built-in ones
          theme.options = {{"wish", "Wish"}, {"light", "Light"}, {"dark", "Dark"}, {"classic", "Classic"}};
        theme.default_value = std::string{"light"};
        theme.restart = true;
        detail::describe_setting_sync(*user_store_proxy_, theme);
      } catch (const std::exception& ex) {
        std::cerr << "[desktop] cannot describe settings: " << ex.what() << '\n';
      }
    }
    desktop_settings saved;
    if (user_store_proxy_) {
      try {
        if (auto entry = detail::user_store_get_sync(*user_store_proxy_, kSettingsEntry)) {
          saved.saved_pins = entry->findField("pinned"_key) != nullptr;
          saved.pinned = string_list(*entry, "pinned"_key);
          saved.on_desktop = string_list(*entry, "desktop"_key);
          saved.theme = entry->get_as<std::string>("theme"_key, std::string{});
        }
      } catch (const std::exception& ex) {
        std::cerr << "[desktop] cannot read saved settings: " << ex.what() << '\n';
      }
    }

    // The configuration file (written with the default content if missing).
    // Its stamp is taken first: an edit made from here on is seen by the
    // watcher below.
    config_stamp_ = file_stamp(config_file_);
    auto loaded = desktop::load_config(config_file_, placeholders());
    if (loaded.wrote_default)
      config_stamp_ = file_stamp(config_file_);
    if (!loaded.error.empty())
      std::cerr << "[desktop] " << loaded.error << '\n';
    // A theme set in the file wins over the one saved from the tray toggle.
    if (!loaded.config.look.theme.empty())
      saved.theme = loaded.config.look.theme;
    {
      auto st = state_.wlock();
      st->settings = saved;
      st->menu = std::move(loaded.config.menu);
      st->look = std::move(loaded.config.look);
      st->taskbar = std::move(loaded.config.taskbar);
      st->config_error = loaded.error;
    }

    // Theme.
    if (!saved.theme.empty())
      apply_theme(saved.theme);

    // Wallpaper: uploaded into the upstream sandbox, then referenced by path.
    refresh_wallpaper();

    template_proxy_ = upstream().instantiate("wish"_key, "__WishTemplate"_key).get();
    build_shell();
    // Clients that connected while the shell was being built.
    post([this] { push_running_state(); });
  } catch (const std::exception& ex) {
    std::cerr << "[desktop] chrome build error: " << ex.what() << '\n';
  }

  // Watch the configuration file (an edit rebuilds the shell) and the
  // wallpaper image (a new image is uploaded).
  if (!config_file_.empty()) {
    watcher_thread_ = std::thread([this, last = config_stamp_, last_wallpaper = wallpaper_stamp_]() mutable {
      for (;;) {
        bool stop = watcher_stop_.wait_for(config_poll_, [](const bool& s) { return s; });
        if (stop)
          return;
        auto now = file_stamp(config_file_);
        if (now && now != last) {
          last = now;
          post([this] { reload_config(); });
        }
        std::filesystem::path wallpaper = state_.rlock()->wallpaper_file;
        auto wallpaper_now = wallpaper.empty() ? std::nullopt : file_stamp(wallpaper);
        if (wallpaper_now != last_wallpaper) {
          last_wallpaper = wallpaper_now;
          post([this] {
            if (refresh_wallpaper())
              build_shell();
          });
        }
      }
    });
  }
}

namespace {

/// Appends @p entries to @p children as DesktopApp / DesktopMenu / Separator
/// descriptors, recording each DesktopApp's dot-path -> entry id.
void describe_entries(
    const std::vector<desktop::menu_entry>& entries,
    nlohmann::ordered_json& children,
    const std::string& prefix,
    const desktop_settings& settings,
    std::map<std::string, std::string>& path_to_id) {
  auto contains = [](const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
  };
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    std::string name = "e" + std::to_string(i);
    std::string path = prefix.empty() ? name : prefix + "." + name;
    nlohmann::ordered_json c;
    switch (e.kind) {
      case desktop::entry_kind::separator:
        c["type"] = "Separator";
        break;
      case desktop::entry_kind::folder: {
        c["type"] = "DesktopMenu";
        c["id"] = e.id;
        c["label"] = e.label;
        c["icon"] = e.icon;
        c["tooltip"] = e.tooltip;
        nlohmann::ordered_json sub = nlohmann::ordered_json::object();
        describe_entries(e.children, sub, path, settings, path_to_id);
        c["children"] = std::move(sub);
        break;
      }
      case desktop::entry_kind::run:
      case desktop::entry_kind::action:
        c["type"] = "DesktopApp";
        c["id"] = e.id;
        c["app"] = e.app;
        c["label"] = e.label;
        c["tooltip"] = e.tooltip;
        c["icon"] = e.icon;
        // The user's own pins win over the file's once they changed any.
        c["pinned"] = settings.saved_pins ? contains(settings.pinned, e.id) : e.pinned;
        c["on_desktop"] = settings.saved_pins ? contains(settings.on_desktop, e.id) : e.on_desktop;
        path_to_id[path] = e.id;
        break;
    }
    children[name] = std::move(c);
  }
}

} // namespace

void wish_desktop::build_shell() {
  if (!template_proxy_)
    return;
  desktop::start_menu menu;
  desktop::appearance look;
  desktop::taskbar_settings taskbar;
  desktop_settings settings;
  std::string error;
  std::string name;
  {
    auto st = state_.wlock();
    menu = st->menu;
    look = st->look;
    taskbar = st->taskbar;
    settings = st->settings;
    error = st->config_error;
    if (!st->wallpaper_error.empty())
      error += (error.empty() ? "" : "\n") + st->wallpaper_error;
    name = std::string(kChromeTemplateName) + "_" + std::to_string(st->templates++);
  }

  nlohmann::ordered_json root;
  root["type"] = "DesktopShell";
  root["user"] = user_;
  root["wallpaper"] = wallpaper_src_;
  root["wallpaper_fit"] = look.wallpaper_fit;
  root["background"] = look.background;
  root["show_icons"] = look.desktop_icons;
  root["time_format"] = taskbar.time_format;
  root["date_format"] = taskbar.date_format;
  root["config_error"] = error;
  nlohmann::ordered_json children = nlohmann::ordered_json::object();
  std::map<std::string, std::string> path_to_id;
  describe_entries(menu.items, children, "", settings, path_to_id);
  if (!menu.power.empty()) {
    nlohmann::ordered_json power;
    power["type"] = "DesktopMenu";
    power["id"] = "power";
    power["role"] = "power";
    power["label"] = "Power";
    nlohmann::ordered_json sub = nlohmann::ordered_json::object();
    describe_entries(menu.power, sub, "power", settings, path_to_id);
    power["children"] = std::move(sub);
    children["power"] = std::move(power);
  }
  root["children"] = std::move(children);

  dynamic reg_args;
  reg_args["name"_key] = key_t{name.c_str()};
  reg_args["descriptor"_key] = dynamic_ptr{import_descriptor_json(root.dump())};
  template_proxy_->call("register"_key, std::move(reg_args)).get();

  dynamic inst_args;
  inst_args["name"_key] = key_t{name.c_str()};
  auto result = template_proxy_->call("instantiate"_key, std::move(inst_args)).get();

  key_t shell_id{0u};
  std::map<std::string, key_t> entry_nodes;
  result.forEach([&](key_t, const field& v) {
    if (!v.is<dynamic_ptr>() || !v.as<dynamic_ptr>())
      return;
    const auto& entry = v.as<dynamic_ptr>();
    const auto* name_field = entry->findField("name"_key);
    const auto* id_field = entry->findField("id"_key);
    if (!name_field || !id_field || !name_field->is<std::string>() || !id_field->is<key_t>())
      return;
    const std::string& path = name_field->as<std::string>();
    if (path.empty())
      shell_id = id_field->as<key_t>();
    else if (auto it = path_to_id.find(path); it != path_to_id.end())
      entry_nodes[it->second] = id_field->as<key_t>();
  });
  if (shell_id.id == 0u)
    throw std::runtime_error("the desktop shell template has no root");

  // Swap in the new shell, then remove the old one (destroying a template
  // root takes it off the screen).
  std::optional<rmi::proxy::dynamic> old = std::move(shell_proxy_);
  shell_proxy_ = upstream().make_proxy(shell_id);
  for (const char* ev : {"activate", "pin", "desktop_icon", "theme"}) {
    key_t event{ev};
    shell_proxy_->onEvent(event, [this, event](dynamic payload) { on_shell_event(event, payload); });
  }
  {
    auto st = state_.wlock();
    st->shell_id = shell_id;
    st->entry_nodes = std::move(entry_nodes);
  }
  if (old) {
    try {
      old->destroy();
    } catch (const std::exception& ex) {
      std::cerr << "[desktop] cannot remove the previous shell: " << ex.what() << '\n';
    }
  }
  push_running_state();
}

void wish_desktop::reload_config() {
  std::string text;
  {
    std::ifstream in(config_file_, std::ios::binary);
    if (!in)
      return; // being replaced by an editor; the next change reloads
    text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  std::string new_theme;
  try {
    auto config = desktop::parse_config_text(text, placeholders());
    auto st = state_.wlock();
    // A theme set in the file applies when it changes; the tray toggle
    // decides in between.
    if (config.look.theme != st->look.theme)
      new_theme = config.look.theme;
    st->menu = std::move(config.menu);
    st->look = std::move(config.look);
    st->taskbar = std::move(config.taskbar);
    st->config_error.clear();
  } catch (const std::exception& ex) {
    // Keep the menu in use; show what's wrong at the top of the Start menu.
    std::string error = config_file_.filename().string() + ": " + ex.what();
    std::cerr << "[desktop] " << error << '\n';
    key_t shell{0u};
    {
      auto st = state_.wlock();
      st->config_error = error;
      shell = st->shell_id;
    }
    if (shell.id != 0u) {
      dynamic fields;
      fields["config_error"_key] = error;
      upstream().make_proxy(shell).set(std::move(fields)).get();
    }
    return;
  }
  if (!new_theme.empty()) {
    dynamic args;
    args["preset"_key] = new_theme;
    run_action("set_theme", args);
  }
  refresh_wallpaper();
  build_shell();
}

bool wish_desktop::refresh_wallpaper() {
  // set_wallpaper() (the --wallpaper flag) wins over the config file.
  std::filesystem::path file = wallpaper_;
  std::string origin = "--wallpaper";
  if (file.empty()) {
    file = state_.rlock()->look.wallpaper;
    origin = "appearance.wallpaper";
  }
  std::string src = wallpaper_src_;
  std::string error;
  if (file.empty()) {
    src.clear();
  } else if (auto stamp = file_stamp(file); !stamp) {
    error = origin + ": cannot read " + file.string();
    src.clear();
  } else if (file != wallpaper_uploaded_ || stamp != wallpaper_stamp_ || src.empty()) {
    try {
      src = upload_wallpaper(file);
      wallpaper_uploaded_ = file;
      wallpaper_stamp_ = stamp;
    } catch (const std::exception& ex) {
      error = origin + ": " + ex.what();
      src.clear();
    }
  }
  if (!error.empty())
    std::cerr << "[desktop] " << error << '\n';
  bool changed = src != wallpaper_src_;
  wallpaper_src_ = src;
  auto st = state_.wlock();
  changed = changed || st->wallpaper_error != error;
  st->wallpaper_error = error;
  st->wallpaper_file = file;
  return changed;
}

std::string wish_desktop::upload_wallpaper(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read " + file.string());
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  std::string src = std::string(kWallpaperDir) + "/wallpaper_" + std::to_string(++wallpaper_uploads_) + ext;
  auto fs = upstream().instantiate("wish"_key, "__WishFileSystem"_key).get();
  dynamic args;
  args["name"_key] = src;
  args["data"_key] = std::move(bytes);
  args["first"_key] = true;
  args["eof"_key] = true;
  fs.call("upload_chunk"_key, std::move(args)).get();
  if (!wallpaper_src_.empty()) {
    try {
      dynamic erase_args;
      erase_args["name"_key] = wallpaper_src_;
      fs.call("erase"_key, std::move(erase_args)).get();
    } catch (...) {
      // Best effort: a stale image in the desktop's private directory.
    }
  }
  return src;
}

// ── Shell events and actions ──

void wish_desktop::on_shell_event(key_t event, const dynamic& payload) {
  const std::string id = payload.get_as<std::string>("id"_key, std::string{});

  if (event == key_t{"activate"}) {
    post([this, id] { launch_entry(id); });
  } else if (event == key_t{"pin"} || event == key_t{"desktop_icon"}) {
    const bool pin = event == key_t{"pin"};
    const bool value = payload.get_as<bool>(pin ? "pinned"_key : "on_desktop"_key, false);
    post([this, id, pin, value] {
      key_t node{0u};
      {
        auto st = state_.wlock();
        auto& s = st->settings;
        if (!s.saved_pins) {
          // First change: start from the configuration file's pins.
          s.saved_pins = true;
          s.pinned.clear();
          s.on_desktop.clear();
          for (const desktop::menu_entry* e : st->menu.all()) {
            if (e->pinned)
              s.pinned.push_back(e->id);
            if (e->on_desktop)
              s.on_desktop.push_back(e->id);
          }
        }
        auto& list = pin ? s.pinned : s.on_desktop;
        list.erase(std::remove(list.begin(), list.end(), id), list.end());
        if (value)
          list.push_back(id);
        if (auto it = st->entry_nodes.find(id); it != st->entry_nodes.end())
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
      dynamic args;
      args["preset"_key] = preset;
      run_action("set_theme", args);
    });
  }
}

dynamic wish_desktop::run_action(const std::string& action, const dynamic& args) {
  dynamic result;
  if (action == "quit") {
    request_quit();
  } else if (action == "shutdown_server") {
    key_t shell = state_.rlock()->shell_id;
    if (shell.id != 0u) {
      dynamic fields;
      fields["shutdown"_key] = true;
      upstream().make_proxy(shell).set(std::move(fields)).get();
    }
  } else if (action == "set_theme" || action == "toggle_theme") {
    std::string preset = args.get_as<std::string>("preset"_key, std::string{});
    if (action == "toggle_theme")
      preset = next_theme_name(state_.rlock()->settings.theme);
    if (preset.empty())
      throw std::runtime_error("set_theme: missing \"preset\"");
    state_.wlock()->settings.theme = preset;
    apply_theme(preset);
    save_settings();
  } else if (action == "reload_config") {
    post([this] { reload_config(); });
  } else if (action == "launch") {
    result["ok"_key] = launch_entry(args.get_as<std::string>("id"_key, std::string{}));
  } else if (action == "items") {
    auto list = std::make_shared<dynamic>();
    uint32_t i = 0;
    for (const desktop::menu_entry* e : state_.rlock()->menu.all()) {
      if (e->kind != desktop::entry_kind::run && e->kind != desktop::entry_kind::action)
        continue;
      auto item = std::make_shared<dynamic>();
      (*item)["id"_key] = e->id;
      (*item)["label"_key] = e->label;
      (*item)["app"_key] = e->app;
      (*list)[key_t{i++}] = dynamic_ptr{item};
    }
    result["items"_key] = dynamic_ptr{list};
  } else {
    throw std::runtime_error("unknown desktop action '" + action + "'");
  }
  return result;
}

std::string wish_desktop::next_theme_name(const std::string& current) const {
  const auto themes = *themes_.rlock();
  if (themes.empty()) // an older server: the two built-in ones that always exist
    return current == "dark" ? "light" : "dark";
  for (size_t i = 0; i < themes.size(); ++i) {
    if (themes[i].name == current)
      return themes[(i + 1) % themes.size()].name;
  }
  return themes.front().name;
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
  if (s.saved_pins) {
    entry["pinned"_key] = string_list_value(s.pinned);
    entry["desktop"_key] = string_list_value(s.on_desktop);
  }
  entry["theme"_key] = s.theme;
  detail::user_store_set_sync(*user_store_proxy_, kSettingsEntry, std::move(entry));
}

bool wish_desktop::launch_entry(const std::string& id) {
  std::optional<desktop::menu_entry> entry;
  {
    auto st = state_.rlock();
    if (const desktop::menu_entry* e = st->menu.find(id))
      entry = *e;
  }
  if (!entry) {
    std::cerr << "[desktop] no Start menu entry '" << id << "'\n";
    return false;
  }
  if (entry->kind == desktop::entry_kind::action) {
    try {
      run_action(entry->action, json_to_dynamic(entry->args));
      return true;
    } catch (const std::exception& ex) {
      std::cerr << "[desktop] " << entry->label << ": " << ex.what() << '\n';
      return false;
    }
  }
  if (entry->kind != desktop::entry_kind::run)
    return false;
  if (!launcher_) {
    std::cerr << "[desktop] cannot start " << entry->label << ": no launcher configured\n";
    return false;
  }
  launch_request req;
  req.argv = entry->argv;
  req.env = launch_env_;
  req.env.insert(req.env.end(), entry->env.begin(), entry->env.end());
  req.cwd = entry->cwd;
  if (!log_dir_.empty()) {
    std::string file = entry->id;
    std::replace_if(file.begin(), file.end(), [](char c) { return c == '/' || c == '\\' || c == ':'; }, '.');
    req.log_path = log_dir_ / (file + "_" + std::to_string(++launch_count_) + ".log");
  }
  std::string error;
  if (!launcher_->launch(req, error)) {
    std::cerr << "[desktop] cannot start " << entry->label << ": " << error << '\n';
    return false;
  }
  return true;
}

// ── __WishDesktop, served to downstream clients ──

bool wish_desktop::on_check_class(rmi::context& ctx, key_t ns, key_t klass) {
  if (ns == "wish"_key && klass == "__WishDesktop"_key)
    return true;
  return bridge::on_check_class(ctx, ns, klass);
}

dynamic_ptr wish_desktop::on_create_object(rmi::context& ctx, key_t ns, key_t klass) {
  if (!(ns == "wish"_key && klass == "__WishDesktop"_key))
    return bridge::on_create_object(ctx, ns, klass);
  auto obj = dynamic_ptr{"__WishDesktop"_rkey, {}};
  // One method per action; each runs it with the call's params.
  for (const std::string& action : desktop::known_actions()) {
    obj->addMethod(key_t{action.c_str()}, method{[this, action](dynamic&, const dynamic& params) -> dynamic {
                     return run_action(action, params);
                   }});
  }
  obj->addMethod(
      "items"_key, method{[this](dynamic&, const dynamic& params) -> dynamic { return run_action("items", params); }});
  return obj;
}

// ── Downstream clients ──

bool wish_desktop::try_handle_request(
    rmi::context& ctx,
    const rmi::shared::envelope& env,
    rmi::transport::server_connection_iface& conn) {
  using namespace rmi::shared::constants;
  if (env.op == OP_CONNECT) {
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
  // The bridge's own objects are answered here by the base server: a new
  // __WishDesktop, and anything sent to an object of this session's own
  // (relayed objects live upstream, never in the downstream context).
  if (env.op == OP_INSTANTIATE) {
    const field* klass = env.payload.findField(FIELD_KLASS);
    if (klass && klass->is<key_t>() && klass->as<key_t>() == "__WishDesktop"_key)
      return false;
  } else if (env.object_id.id != 0u && ctx.objects.count(env.object_id.id) != 0) {
    return false;
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

void wish_desktop::add_unknown_apps() {
  if (!template_proxy_)
    return;
  std::vector<std::pair<std::string, std::pair<std::string, std::string>>> unknown;
  {
    auto st = state_.rlock();
    std::vector<const desktop::menu_entry*> entries = st->menu.all();
    for (const auto& [session, app] : st->session_apps) {
      bool known = st->extra_nodes.count(app) != 0 ||
          std::any_of(entries.begin(), entries.end(), [&](const auto* e) { return app_matches(e->app, app); });
      if (known)
        continue;
      if (std::any_of(unknown.begin(), unknown.end(), [&](const auto& u) { return u.first == app; }))
        continue; // two sessions of the same new app
      auto meta = st->app_meta.find(app);
      unknown.emplace_back(app, meta != st->app_meta.end() ? meta->second : std::pair<std::string, std::string>{});
    }
  }
  for (const auto& [app, meta] : unknown) {
    std::string label = meta.first;
    if (label.empty())
      label = app.substr(app.rfind('/') == std::string::npos ? 0 : app.rfind('/') + 1);
    nlohmann::ordered_json d;
    d["type"] = "DesktopApp";
    d["id"] = app;
    d["app"] = app;
    d["label"] = label;
    d["icon"] = meta.second;
    d["listed"] = false;
    // A one-off template per app: instantiating it adds a top-level
    // DesktopApp next to the shell, which the server lists after the
    // shell's own entries (see render_desktop_shell()). It survives shell
    // rebuilds.
    std::string name = std::string(kChromeTemplateName) + "_" + std::to_string(state_.wlock()->templates++);
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
      state_.wlock()->extra_nodes[app] = node;
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
    // Every entry node, so one whose app stopped is cleared too.
    for (const auto& [id, node] : st->entry_nodes) {
      auto& sessions = by_node[node];
      const desktop::menu_entry* e = st->menu.find(id);
      if (!e)
        continue;
      for (const auto& [session, app] : st->session_apps)
        if (app_matches(e->app, app))
          sessions.push_back(static_cast<int32_t>(session));
    }
    for (const auto& [app, node] : st->extra_nodes) {
      auto& sessions = by_node[node];
      for (const auto& [session, session_app] : st->session_apps)
        if (session_app == app)
          sessions.push_back(static_cast<int32_t>(session));
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

// ── Single instance ──────────────────────────────────────────────────────────

std::unique_ptr<bison::rmi::transport::named_pipe_server_transport> claim_desktop_endpoint(const std::string& name) {
  auto endpoint = std::make_unique<bison::rmi::transport::named_pipe_server_transport>(name);
  try {
    endpoint->start(bison::dynamic{});
  } catch (const bison::rmi::transport::address_in_use_error&) {
    return nullptr;
  }
  return endpoint;
}

// ── wish_desktop_app ─────────────────────────────────────────────────────────

int wish_desktop_app::run(int argc, char** argv) {
  apply_env_flag_defaults();
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // One desktop per machine: owning the fixed endpoint is the lock. Checked
  // before anything else starts (upstream server, terminal, launcher).
  // When the downstream transport is that same pipe, bridge_app binds it
  // and fails the same way, so no extra listener is needed.
  const std::string endpoint_name = desktop_endpoint();
  if (FLAGS_downstream_transport != "pipe" || FLAGS_downstream_name != endpoint_name) {
    try {
      endpoint_ = claim_desktop_endpoint(endpoint_name);
    } catch (const std::exception& e) {
      std::cerr << "[desktop] cannot listen on " << endpoint_name << ": " << e.what() << '\n';
      return 1;
    }
    if (!endpoint_) {
      std::cerr << "[desktop] wish desktop is already running (" << endpoint_name << ")\n";
      return 1;
    }
  }

  set_env_var("WISH_TRANSPORT", FLAGS_downstream_transport);
  set_env_var("WISH_HOST", FLAGS_downstream_host);
  set_env_var("WISH_PORT", std::to_string(FLAGS_downstream_port));
  set_env_var("WISH_NAME", FLAGS_downstream_name);

  int rc = bison::app::bridge_app::run(argc, argv);
  // The bridge, the server's only client, is gone: stop the server.
  server_.reset();
  server_transport_.reset();
  return rc;
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
  bison::rmi::transport::server_transport_iface* downstream = &downstream_transport;
  if (endpoint_) {
    downstream_ = std::make_unique<multi_server_transport>(downstream_transport);
    downstream_->add(std::move(endpoint_));
    downstream = downstream_.get();
  }
  auto br = std::make_unique<wish_desktop>(*downstream, std::move(upstream_transport), std::move(upstream_params));

  // {wish_client} in the configuration, also when nothing can be started.
  br->set_client_command(client_command_);
  if (FLAGS_downstream_transport == "term") {
    std::cerr << "[desktop] --downstream_transport=term: apps cannot be launched from the Start menu\n";
  } else {
    if (!launcher_)
      launcher_ = std::make_unique<process_launcher>();
    br->set_launcher(launcher_.get());
    br->set_launch_env({
        {"WISH_TRANSPORT", FLAGS_downstream_transport},
        {"WISH_HOST", connectable_host(FLAGS_downstream_host)},
        {"WISH_PORT", std::to_string(FLAGS_downstream_port)},
        {"WISH_NAME", FLAGS_downstream_name},
    });
    br->set_log_dir(wish_home{FLAGS_wish_home}.logs_dir() / "desktop");
  }
  if (!FLAGS_wallpaper.empty())
    br->set_wallpaper(FLAGS_wallpaper);
  const std::string user = FLAGS_upstream_username.empty() ? os_user_name() : FLAGS_upstream_username;
  br->set_config_file(FLAGS_config.empty() ? desktop_config_path(wish_home{FLAGS_wish_home}, user)
                                           : std::filesystem::path(FLAGS_config));
  br->set_user(user);

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

std::unique_ptr<bison::rmi::transport::client_transport_iface> wish_desktop_app::make_upstream_transport() {
  if (!FLAGS_with_server)
    return nullptr;
  if (!server_factory_)
    throw std::runtime_error("--with_server is only available in the combined wish binary");
  server_transport_ = std::make_unique<rmi::transport::memory_server_transport>();
  server_ = server_factory_(*server_transport_);
  return std::make_unique<rmi::transport::memory_client_transport>(server_transport_->connect());
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
    // --with_server: its window was closed, or "Shut down server" was used.
    if (server_ && server_->should_quit())
      break;
    // Without its server the desktop can show nothing, yet it would keep
    // the fixed endpoint, refusing a new desktop and leaving the clients
    // that connect to it unanswered.
    if (!desktop_->upstream_connected()) {
      std::cerr << "[desktop] lost the connection to the wish server; exiting\n";
      break;
    }
  }
  // Apps launched from the desktop go down with it.
  if (launcher_)
    launcher_->terminate_all();
  // Before bridge_app stops the bridge (and its upstream connection).
  desktop_->stop_tasks();
}

} // namespace bdg::wish
