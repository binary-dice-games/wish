// MIT License © 2025 Binary Dice Games
//
// Exercises apply_env_flag_defaults() (app/wish_cli/env_flags.hpp): a
// WISH_<FLAG_NAME_UPPERCASED> environment variable should become a
// registered gflags flag's default value, but an already explicitly-set
// flag value (standing in for a command-line-supplied value, which is
// always parsed after apply_env_flag_defaults() runs) must be unaffected.
#include "app/wish_cli/env_flags.hpp"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <cstdlib>

DEFINE_string(test_env_flag_str, "compiled_default", "env_flags test flag (string)");
DEFINE_int32(test_env_flag_int, 5, "env_flags test flag (int)");

namespace wish = bdg::wish;

namespace {

gflags::CommandLineFlagInfo flag_info(const char* name) {
  gflags::CommandLineFlagInfo info;
  EXPECT_TRUE(gflags::GetCommandLineFlagInfo(name, &info));
  return info;
}

} // namespace

TEST(EnvFlagsTest, EnvVarBecomesFlagDefaultWhenSet) {
  setenv("WISH_TEST_ENV_FLAG_STR", "from_env", 1);
  wish::apply_env_flag_defaults();
  unsetenv("WISH_TEST_ENV_FLAG_STR");

  auto info = flag_info("test_env_flag_str");
  EXPECT_EQ(info.default_value, "from_env");
  EXPECT_EQ(info.current_value, "from_env");

  // Restore, so this test doesn't leak state into other tests in this binary.
  gflags::SetCommandLineOptionWithMode("test_env_flag_str", "compiled_default", gflags::SET_FLAGS_DEFAULT);
}

TEST(EnvFlagsTest, NoEnvVarLeavesFlagAtCompiledDefault) {
  unsetenv("WISH_TEST_ENV_FLAG_INT");
  wish::apply_env_flag_defaults();

  auto info = flag_info("test_env_flag_int");
  EXPECT_EQ(info.default_value, "5");
  EXPECT_EQ(info.current_value, "5");
}

TEST(EnvFlagsTest, ExplicitlySetFlagValueOutranksEnvVar) {
  // Stands in for a command-line-supplied value: SET_FLAGS_VALUE (unlike
  // SET_FLAGS_DEFAULT) marks the flag as no longer at its default, so a
  // later apply_env_flag_defaults() call changes only the default bucket,
  // not the already-set current value -- mirroring "CLI flag always wins".
  gflags::SetCommandLineOptionWithMode("test_env_flag_int", "42", gflags::SET_FLAGS_VALUE);

  setenv("WISH_TEST_ENV_FLAG_INT", "999", 1);
  wish::apply_env_flag_defaults();
  unsetenv("WISH_TEST_ENV_FLAG_INT");

  auto info = flag_info("test_env_flag_int");
  EXPECT_EQ(info.current_value, "42");
  EXPECT_EQ(info.default_value, "999");

  // Restore.
  gflags::SetCommandLineOptionWithMode("test_env_flag_int", "5", gflags::SET_FLAGS_DEFAULT);
}
