// MIT License © 2026 Binary Dice Games
/// @file os_user.hpp
/// @brief `wish::os_user_name()`: the logged-in OS user, wish's default
///        session identity. See src/context/DESIGN.md.
#pragma once

#include <string>

namespace bdg::wish {

/**
 * @brief The login name of the user running this process.
 *
 * POSIX: the password database entry of the effective uid, falling back to
 * `$USER`/`$LOGNAME`. Native Windows: `GetUserNameW()`, falling back to
 * `%USERNAME%`. A `DOMAIN\user` or `user@domain` form keeps only `user`.
 *
 * Not normalized (see `normalize_identity()`); the server does that.
 *
 * @return The name, or empty if it cannot be determined.
 */
std::string os_user_name();

} // namespace bdg::wish
