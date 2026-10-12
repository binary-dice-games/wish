// MIT License © 2026 Binary Dice Games
/**
 * @file desktop_endpoint.hpp
 * @brief Name of the fixed local endpoint `wish desktop` listens on.
 *
 * There is at most one desktop per machine. It always listens on this
 * system-wide named pipe / abstract socket, and owning the name is what
 * keeps a second desktop from starting (see app/wish_cli/desktop/PLAN.md).
 * Clients connect to it with a `named_pipe_client_transport`.
 */
#pragma once

#include <string>

namespace bdg::wish {

/// @brief Environment variable that overrides desktop_endpoint().
inline constexpr const char* kDesktopEndpointEnv = "WISH_DESKTOP_ENDPOINT";

/**
 * @brief Name of the desktop's fixed local endpoint.
 *
 * @return `$WISH_DESKTOP_ENDPOINT` if set and non-empty; otherwise
 *         `\\.\pipe\wish-desktop` on Windows (including MSYS2) and the
 *         abstract socket `@wish-desktop` elsewhere. The result can be
 *         passed as-is to `named_pipe_server_transport` /
 *         `named_pipe_client_transport` or to `wish client --name`.
 */
std::string desktop_endpoint();

} // namespace bdg::wish
