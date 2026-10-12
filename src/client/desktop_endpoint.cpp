// MIT License © 2026 Binary Dice Games
/// @file desktop_endpoint.cpp
/// @brief `desktop_endpoint()`, in the client library so both `wish desktop`
///        and every client agree on the name.
#include <client/desktop_endpoint.hpp>

#include <cstdlib>

namespace bdg::wish {

std::string desktop_endpoint() {
  const char* overridden = std::getenv(kDesktopEndpointEnv);
  if (overridden && *overridden)
    return overridden;
#if defined(_WIN32) || defined(__CYGWIN__)
  return R"(\\.\pipe\wish-desktop)";
#else
  return "@wish-desktop";
#endif
}

} // namespace bdg::wish
