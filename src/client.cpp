// MIT License © 2025 Binary Dice Games
/// @file client.cpp
/// @brief Implementation of wish::client.
#include <wish/client.hpp>

#include "src/bison/bison.hpp"

#include <future>
#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── client ────────────────────────────────────────────────────────────────────

void client::run() {
  connect();
  try {
    on_session();
  } catch (...) {
    disconnect();
    throw;
  }
  disconnect();
}

// ── Helper: build proxy_map from an indexed apply_descriptor result ───────────

static proxy_map proxies_from_result(
    bison::rmi::client& c,
    bison::dynamic& result) {
  proxy_map pm;
  result.forEach([&pm, &c](key_t /*k*/, const field& v) {
    if (!v.is<dynamic_ptr>()) return;
    const auto& ptr = v.as<dynamic_ptr>();
    if (!ptr) return;
    const auto* nf = ptr->findField("name"_key);
    const auto* idf = ptr->findField("id"_key);
    if (!nf || !idf || !nf->is<std::string>() || !idf->is<key_t>()) return;
    pm.emplace(nf->as<std::string>(), c.make_proxy(idf->as<key_t>()));
  });
  return pm;
}

// ── Wish helpers ──────────────────────────────────────────────────────────────

std::future<proxy_map> client::import_ui(const std::string& descriptor) {
  return std::async(std::launch::async, [this, descriptor]() -> proxy_map {
    auto handler = instantiate("wish"_key, "__WishImport"_key).get();
    dynamic args;
    args["descriptor"_key] = descriptor;
    auto result = handler.call("import"_key, std::move(args)).get();
    return proxies_from_result(*this, result);
  });
}

std::future<void> client::register_template(
    bison::key_t name, const std::string& descriptor) {
  return std::async(std::launch::async, [this, name, descriptor]() {
    auto handler = instantiate("wish"_key, "__WishTemplate"_key).get();
    dynamic args;
    args["name"_key] = name;
    args["descriptor"_key] = descriptor;
    handler.call("register"_key, std::move(args)).get();
  });
}

std::future<proxy_map> client::instantiate_template(bison::key_t name) {
  return std::async(std::launch::async, [this, name]() -> proxy_map {
    auto handler = instantiate("wish"_key, "__WishTemplate"_key).get();
    dynamic args;
    args["name"_key] = name;
    auto result = handler.call("instantiate"_key, std::move(args)).get();
    return proxies_from_result(*this, result);
  });
}

std::future<void> client::upload_file(
    const std::string& name, const std::string& data) {
  return std::async(std::launch::async, [this, name, data]() {
    auto fs = instantiate("wish"_key, "__WishFS"_key).get();
    dynamic args;
    args["name"_key] = name;
    args["data"_key] = data;
    fs.call("upload"_key, std::move(args)).get();
  });
}

std::future<std::string> client::download_file(const std::string& name) {
  return std::async(std::launch::async, [this, name]() -> std::string {
    auto fs = instantiate("wish"_key, "__WishFS"_key).get();
    dynamic args;
    args["name"_key] = name;
    auto result = fs.call("download"_key, std::move(args)).get();
    return result.as<std::string>("result"_key);
  });
}

}  // namespace bdg::wish
