// MIT License © 2025 Binary Dice Games
/// @file local_auth_module.cpp
/// @brief Implementation of wish::local_auth_module.
#include <auth/local_auth_module.hpp>

namespace bdg::wish {

using namespace bdg::bison;

bool local_auth_module::authenticate(bison::rmi::context& ctx, const bison::dynamic& payload,
    std::string& out_identity) {
  (void)ctx;
  const auto* f = payload.findField("username"_key);
  if (f && f->is<std::string>())
    out_identity = f->as<std::string>();
  return true;
}

} // namespace bdg::wish
