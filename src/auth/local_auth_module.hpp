// MIT License © 2025 Binary Dice Games
/// @file local_auth_module.hpp
/// @brief Trust-the-client auth module for local/single-user deployments.
#pragma once

#include "src/rmi/server/auth.hpp"

#include <string>

namespace bdg::wish {

/**
 * @brief Convenience `auth_module_iface` for trusted, local, or single-user
 *        deployments -- see `src/auth/DESIGN.md`.
 *
 * Accepts every connection unconditionally; extracts a client-supplied
 * `"username"_key` field from the connect payload as the identity, without
 * verifying the client's claim in any way. Pass an instance to
 * `server::start()` together with `server::set_persistent_sandbox_root()` to
 * give each distinct username its own persistent sandbox directory.
 *
 * Do not use where clients are untrusted or remote: anything that needs real
 * credential verification (e.g. checking a signed token) should supply its
 * own `bison::rmi::auth_module_iface` implementation instead.
 */
class local_auth_module : public bison::rmi::auth_module_iface {
 public:
  bool authenticate(bison::rmi::context& ctx, const bison::dynamic& payload, std::string& out_identity) override;
};

} // namespace bdg::wish
