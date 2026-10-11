// MIT License © 2025 Binary Dice Games
/**
 * @file env_flags.hpp
 * @brief Environment-variable defaults for wish CLI gflags flags.
 */
#pragma once

#include <string>

namespace bdg::wish {

/**
 * @brief Name of the environment variable that sets @p flag_name's default:
 *        `WISH_<FLAG_NAME_UPPERCASED>` (e.g. `font_size` -> `WISH_FONT_SIZE`).
 */
std::string env_flag_var_name(const std::string& flag_name);

/**
 * @brief Applies an environment-variable default for every gflags flag
 *        currently registered in this process.
 *
 * For each registered flag (e.g. `transport`, `host`, `port`, `name`,
 * `downstream_port`, `web_bind`, ...), if the environment variable
 * `WISH_<FLAG_NAME_UPPERCASED>` is set (e.g. `WISH_TRANSPORT`, `WISH_HOST`,
 * `WISH_PORT`, `WISH_NAME`, `WISH_DOWNSTREAM_PORT`, `WISH_WEB_BIND`, ...), it
 * becomes that flag's default (`gflags::SET_FLAGS_DEFAULT`) -- an explicit
 * command-line value for that flag still overrides it.
 *
 * @note Must be called before `gflags::ParseCommandLineFlags()`.
 */
void apply_env_flag_defaults();

} // namespace bdg::wish
