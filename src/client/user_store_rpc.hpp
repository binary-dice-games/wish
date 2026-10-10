// MIT License © 2025 Binary Dice Games
/// @file user_store_rpc.hpp
/// @brief Client-side calls into the server's `__WishUserStore` service,
///        shared by `wish::client` and `wish::standalone`.
#pragma once

#include "src/bison/bison_object.hpp"
#include "src/rmi/client/proxy.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish::detail {

/// @brief Throws `std::logic_error` if the session has no user store proxy
///        (anonymous session). Called inside the returned future's task, so
///        the error surfaces through `.get()` like any other RMI failure.
inline void require_user_store(const std::optional<bison::rmi::proxy::dynamic>& proxy) {
  if (!proxy)
    throw std::logic_error(
        "wish: user store unavailable: anonymous session (connect with an identity, e.g. --username; "
        "see has_user_store())");
}

/// @brief Synchronous `get`; `std::nullopt` if the entry doesn't exist.
inline std::optional<bison::dynamic> user_store_get_sync(bison::rmi::proxy::dynamic& proxy, const std::string& name) {
  using namespace bison;
  dynamic args;
  args["name"_key] = name;
  auto result = proxy.call("get"_key, std::move(args)).get();
  if (!result.get_as<bool>("found"_key, false))
    return std::nullopt;
  const auto* value = result.findField("value"_key);
  if (!value || !value->is<dynamic_ptr>() || !value->as<dynamic_ptr>())
    return dynamic{};
  return *value->as<dynamic_ptr>();
}

/// @brief Synchronous `set`.
inline void user_store_set_sync(bison::rmi::proxy::dynamic& proxy, const std::string& name, bison::dynamic value) {
  using namespace bison;
  dynamic args;
  args["name"_key] = name;
  args["value"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(value))};
  proxy.call("set"_key, std::move(args)).get();
}

/// @brief Synchronous `erase`; `true` if the entry existed.
inline bool user_store_erase_sync(bison::rmi::proxy::dynamic& proxy, const std::string& name) {
  using namespace bison;
  dynamic args;
  args["name"_key] = name;
  auto result = proxy.call("erase"_key, std::move(args)).get();
  return result.get_as<bool>("erased"_key, false);
}

/// @brief Synchronous `keys`; entry names, sorted.
inline std::vector<std::string> user_store_keys_sync(bison::rmi::proxy::dynamic& proxy) {
  using namespace bison;
  auto result = proxy.call("keys"_key, dynamic{}).get();
  std::vector<std::string> names;
  result.forEach([&names](key_t, const field& f) {
    if (f.is<std::string>())
      names.push_back(f.as<std::string>());
  });
  return names;
}

} // namespace bdg::wish::detail
