// MIT License © 2025 Binary Dice Games
/**
 * @file env_flags.cpp
 * @brief Environment-variable defaults for wish CLI gflags flags.
 */
#include "app/wish_cli/env_flags.hpp"

#include <gflags/gflags.h>

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace bdg::wish {

std::string env_flag_var_name(const std::string& flag_name) {
  std::string env_name = "WISH_";
  for (char c : flag_name)
    env_name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return env_name;
}

void apply_env_flag_defaults() {
  std::vector<gflags::CommandLineFlagInfo> flags;
  gflags::GetAllFlags(&flags);

  for (const auto& flag : flags) {
    const std::string env_name = env_flag_var_name(flag.name);
    const char* value = std::getenv(env_name.c_str());
    if (value != nullptr)
      gflags::SetCommandLineOptionWithMode(flag.name.c_str(), value, gflags::SET_FLAGS_DEFAULT);
  }
}

} // namespace bdg::wish
