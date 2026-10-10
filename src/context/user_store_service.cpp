// MIT License © 2025 Binary Dice Games
/// @file user_store_service.cpp
/// @brief Implementation of wish::user_store_service.
#include <context/user_store_service.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace bdg::wish {

using namespace bison;

user_store_service_ptr user_store_service::instantiate(persistent_store_ptr store) {
  return std::make_shared<user_store_service>(
      dynamic::instantiate(key_t{"wish"}, key_t{"__WishUserStore"}), std::move(store));
}

user_store_service::user_store_service(dynamic&& base, persistent_store_ptr store)
    : dynamic(std::move(base)), store_(std::move(store)) {
  if (!store_)
    throw std::invalid_argument("wish::user_store_service: null store");

  addMethod("get"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              dynamic result;
              auto value = store_->get(p.as<std::string>("name"_key));
              result["found"_key] = value.has_value();
              if (value)
                result["value"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(*value))};
              return result;
            }});
  addMethod("set"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              const auto* value = p.findField("value"_key);
              if (!value || !value->is<dynamic_ptr>() || !value->as<dynamic_ptr>())
                throw std::invalid_argument("wish::user_store_service: set requires an object \"value\"");
              store_->set(p.as<std::string>("name"_key), *value->as<dynamic_ptr>());
              return dynamic{};
            }});
  addMethod("erase"_key, method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              dynamic result;
              result["erased"_key] = store_->erase(p.as<std::string>("name"_key));
              return result;
            }});
  addMethod("keys"_key, method{[this](dynamic& /*self*/, const dynamic& /*p*/) -> dynamic {
              dynamic result;
              std::size_t idx = 0;
              for (auto& name : store_->keys())
                result[idx++] = std::move(name);
              return result;
            }});
}

void register_user_store_service() {
  auto proto = dynamic_ptr{"__WishUserStore"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish
