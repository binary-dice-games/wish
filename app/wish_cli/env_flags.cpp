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

void apply_env_flag_defaults() {
  std::vector<gflags::CommandLineFlagInfo> flags;
  gflags::GetAllFlags(&flags);

  for (const auto& flag : flags) {
    std::string env_name = "WISH_";
    for (char c : flag.name)
      env_name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    const char* value = std::getenv(env_name.c_str());
    if (value != nullptr)
      gflags::SetCommandLineOptionWithMode(flag.name.c_str(), value, gflags::SET_FLAGS_DEFAULT);
  }
}

} // namespace bdg::wish
