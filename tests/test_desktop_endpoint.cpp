// MIT License © 2026 Binary Dice Games
//
// Exercises desktop_endpoint() (src/client/desktop_endpoint.hpp): the fixed
// name `wish desktop` listens on, and its WISH_DESKTOP_ENDPOINT override.
// ctest runs each test in its own process, so changing the environment here
// cannot affect other tests.
#include <client/desktop_endpoint.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

namespace wish = bdg::wish;

namespace {

// MSVC lacks the POSIX setenv/unsetenv names; _putenv_s is the equivalent,
// and an empty value removes the variable just like unsetenv.
void set_env(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#if defined(_WIN32)
  _putenv_s(name, "");
#else
  unsetenv(name);
#endif
}

} // namespace

TEST(DesktopEndpoint, DefaultNameIsPlatformWide) {
  unset_env(wish::kDesktopEndpointEnv);
#if defined(_WIN32) || defined(__CYGWIN__)
  EXPECT_EQ(wish::desktop_endpoint(), R"(\\.\pipe\wish-desktop)");
#else
  EXPECT_EQ(wish::desktop_endpoint(), "@wish-desktop");
#endif
}

TEST(DesktopEndpoint, EnvironmentOverridesName) {
  set_env(wish::kDesktopEndpointEnv, "@wish-desktop-test");
  EXPECT_EQ(wish::desktop_endpoint(), "@wish-desktop-test");
  unset_env(wish::kDesktopEndpointEnv);
}

TEST(DesktopEndpoint, EmptyEnvironmentValueUsesDefault) {
  unset_env(wish::kDesktopEndpointEnv);
  const std::string default_name = wish::desktop_endpoint();
  set_env(wish::kDesktopEndpointEnv, "");
  EXPECT_EQ(wish::desktop_endpoint(), default_name);
  unset_env(wish::kDesktopEndpointEnv);
}
