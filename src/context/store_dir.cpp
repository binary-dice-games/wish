// MIT License © 2026 Binary Dice Games
/// @file store_dir.cpp
/// @brief `default_store_dir()`, in the client library so client-only
///        programs (e.g. `wish-desktop`) can locate `~/.wish` too.
#include <context/store_dir.hpp>

#include <cstdlib>

namespace bdg::wish {

std::filesystem::path default_store_dir() {
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home && *home)
    return std::filesystem::path(home) / ".wish";
  return std::filesystem::path(".wish");
}

} // namespace bdg::wish
