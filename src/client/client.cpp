// MIT License © 2025 Binary Dice Games
/// @file client.cpp
/// @brief Implementation of wish::client.
#include <client/client.hpp>

#include <ui/ui_descriptor.hpp>

#include "src/bison/bison.hpp"

#include <future>

namespace bdg::wish {

using namespace bison;

// ── client ────────────────────────────────────────────────────────────────────

void client::run(bison::dynamic connect_params) {
  connect(std::move(connect_params));
  try {
    on_session();
  } catch (...) {
    disconnect();
    throw;
  }
  disconnect();
}

void client::on_connect() {
  template_proxy_ = instantiate("wish"_key, "__WishTemplate"_key).get();
  fs_proxy_ = instantiate("wish"_key, "__WishFileSystem"_key).get();
  style_proxy_ = instantiate("wish"_key, "__WishStyle"_key).get();
  log_proxy_ = instantiate("wish"_key, "__WishLogger"_key).get();
}

void client::on_disconnect() {
  template_proxy_.reset();
  fs_proxy_.reset();
  style_proxy_.reset();
  log_proxy_.reset();
}

// ── Helper: build proxy_map from an indexed apply_descriptor result ───────────

static proxy_map proxies_from_result(bison::rmi::client& c, bison::dynamic& result) {
  proxy_map pm;
  result.forEach([&pm, &c](key_t /*k*/, const field& v) {
    if (!v.is<dynamic_ptr>())
      return;
    const auto& ptr = v.as<dynamic_ptr>();
    if (!ptr)
      return;
    const auto* nf = ptr->findField("name"_key);
    const auto* idf = ptr->findField("id"_key);
    if (!nf || !idf || !nf->is<std::string>() || !idf->is<key_t>())
      return;
    pm.emplace(nf->as<std::string>(), c.make_proxy(idf->as<key_t>()));
  });
  return pm;
}

// ── Wish helpers ──────────────────────────────────────────────────────────────

std::future<void> client::register_template(bison::key_t name, bison::dynamic descriptor) {
  return std::async(std::launch::async, [this, name, d = std::move(descriptor)]() mutable {
    dynamic args;
    args["name"_key] = name;
    args["descriptor"_key] = dynamic_ptr{std::move(d)};
    template_proxy_->call("register"_key, std::move(args)).get();
  });
}

std::future<void> client::register_template_from_json(bison::key_t name, const std::string& json) {
  return register_template(name, import_descriptor_json(json));
}

std::future<void> client::register_template_from_yaml(bison::key_t name, const std::string& yaml) {
  return register_template(name, import_descriptor_yaml(yaml));
}

std::future<proxy_map> client::instantiate_template(bison::key_t name) {
  return std::async(std::launch::async, [this, name]() -> proxy_map {
    dynamic args;
    args["name"_key] = name;
    auto result = template_proxy_->call("instantiate"_key, std::move(args)).get();
    return proxies_from_result(*this, result);
  });
}

std::future<void> client::upload_file(const std::string& name, const std::string& data) {
  return std::async(std::launch::async, [this, name, data]() {
    dynamic args;
    args["name"_key] = name;
    args["data"_key] = data;
    fs_proxy_->call("upload"_key, std::move(args)).get();
  });
}

std::future<std::string> client::download_file(const std::string& name) {
  return std::async(std::launch::async, [this, name]() -> std::string {
    dynamic args;
    args["name"_key] = name;
    auto result = fs_proxy_->call("download"_key, std::move(args)).get();
    return result.as<std::string>("result"_key);
  });
}

std::future<std::vector<std::string>> client::list_files(const std::string& path) {
  return std::async(std::launch::async, [this, path]() -> std::vector<std::string> {
    dynamic args;
    if (!path.empty())
      args["path"_key] = path;
    auto result = fs_proxy_->call("list"_key, std::move(args)).get();
    std::vector<std::string> names;
    result.forEach([&names](key_t, const field& f) {
      if (f.is<std::string>())
        names.push_back(f.as<std::string>());
    });
    return names;
  });
}

std::future<void> client::set_style_preset(const std::string& name) {
  return std::async(std::launch::async, [this, name]() {
    dynamic args;
    args["name"_key] = name;
    // oneway=true: server applies the preset but sends no response.
    // This lets the call be made safely from within event callbacks
    // (which run on the RMI worker thread that would otherwise deadlock
    // waiting for a response that only it can deliver).
    style_proxy_->call("preset"_key, std::move(args), true).get();
  });
}

std::future<void> client::set_style(bison::dynamic params) {
  return std::async(std::launch::async, [this, p = std::move(params)]() mutable {
    // oneway=true: same reasoning as set_style_preset above.
    style_proxy_->call("set"_key, std::move(p), true).get();
  });
}

std::future<bison::dynamic> client::get_style() {
  return std::async(std::launch::async, [this]() -> dynamic { return style_proxy_->call("get"_key, dynamic{}).get(); });
}

std::future<void> client::log(const std::string& level, const std::string& msg) {
  return std::async(std::launch::async, [this, level, msg]() {
    dynamic args;
    args["level"_key] = level;
    args["msg"_key] = msg;
    // oneway=true: log messages are fire-and-forget to avoid blocking callers.
    log_proxy_->call("log"_key, std::move(args), true).get();
  });
}

std::future<void> client::log_debug(const std::string& msg) {
  return log("debug", msg);
}
std::future<void> client::log_info(const std::string& msg) {
  return log("info", msg);
}
std::future<void> client::log_warn(const std::string& msg) {
  return log("warn", msg);
}
std::future<void> client::log_error(const std::string& msg) {
  return log("error", msg);
}

} // namespace bdg::wish
