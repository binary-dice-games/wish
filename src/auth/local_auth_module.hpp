// MIT License © 2025 Binary Dice Games
/// @file local_auth_module.hpp
/// @brief Trust-the-client auth module for local/single-user deployments.
#pragma once

#include "src/rmi/server/auth.hpp"

#include <string>
#include <utility>

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
 * If constructed with a non-empty @p default_identity, a client that supplies
 * no username gets that identity instead of none (so it also gets a
 * persistent directory) -- used by `wish server --sandbox_root` so clients
 * that never send a username still share one persistent sandbox.
 *
 * Do not use where clients are untrusted or remote: anything that needs real
 * credential verification (e.g. checking a signed token) should supply its
 * own `bison::rmi::auth_module_iface` implementation instead.
 */
class local_auth_module : public bison::rmi::auth_module_iface {
 public:
  /// @param default_identity Identity used when the client sends no
  ///        `"username"` (empty, the default, means "no identity").
  explicit local_auth_module(std::string default_identity = {}) : default_identity_(std::move(default_identity)) {}

  bool authenticate(bison::rmi::context& ctx, const bison::dynamic& payload, std::string& out_identity) override;

 private:
  std::string default_identity_;
};

} // namespace bdg::wish
