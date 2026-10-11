// MIT License © 2025 Binary Dice Games
//
// Exercises app/wish_cli/config_flags.hpp: the wish CLI resolves each flag
// from the command line, then its WISH_<FLAG> env var, then the server
// store entry saved with --save_defaults, then the hardcoded default.
#include "app/wish_cli/config_flags.hpp"
#include "app/wish_cli/env_flags.hpp"

#include <context/persistent_store.hpp>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

DEFINE_string(test_cfg_cli, "hard_cli", "config_flags test flag");
DEFINE_string(test_cfg_env, "hard_env", "config_flags test flag");
DEFINE_string(test_cfg_store, "hard_store", "config_flags test flag");
DEFINE_string(test_cfg_none, "hard_none", "config_flags test flag");
DEFINE_int32(test_cfg_int, 5, "config_flags test flag");
DEFINE_bool(test_cfg_bool, false, "config_flags test flag");
DEFINE_string(test_cfg_unlisted, "hard_unlisted", "config_flags test flag, excluded from saving");
DEFINE_string(test_cfg_key_password, "", "config_flags test flag, a secret");
DEFINE_string(test_cfg_cert_pem, "", "config_flags test flag, a secret");

DECLARE_bool(save_defaults);
DECLARE_bool(clear_defaults);

using namespace bdg::bison;
namespace wish = bdg::wish;

namespace {

// The test flags that set environment variables (cleared in TearDown).
const std::vector<std::string> kEnvFlags = {"test_cfg_cli", "test_cfg_env", "test_cfg_store"};
// The mode's own exclusions: everything else registered can be saved.
const std::vector<std::string> kExcluded = {"test_cfg_unlisted"};
const std::string kEntry = "wish.cli.test";

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

class ConfigFlagsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string{"wish_config_flags_test_"} + info->name());
    std::filesystem::remove_all(dir_);
  }

  void TearDown() override {
    for (const auto& name : kEnvFlags)
      unset_env(wish::env_flag_var_name(name).c_str());
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path store_file() const {
    return dir_ / "server_store.bison";
  }

  /// Runs the CLI startup sequence: env defaults, then the command line.
  static void parse(std::vector<std::string> args) {
    args.insert(args.begin(), "wish");
    std::vector<char*> argv;
    for (auto& arg : args)
      argv.push_back(arg.data());
    int argc = static_cast<int>(argv.size());
    char** argv_ptr = argv.data();
    wish::apply_env_flag_defaults();
    gflags::ParseCommandLineFlags(&argc, &argv_ptr, true);
  }

  std::filesystem::path dir_;
  gflags::FlagSaver saver_; // restores every flag after each test
};

} // namespace

// ── Saveable flags ───────────────────────────────────────────────────────────

TEST_F(ConfigFlagsTest, EveryRegisteredFlagIsSaveableExceptExclusions) {
  const auto names = wish::config_flag_names(kExcluded);
  auto has = [&](const char* name) { return std::find(names.begin(), names.end(), name) != names.end(); };

  // Registered flags need no listing to be saveable.
  EXPECT_TRUE(has("test_cfg_cli"));
  EXPECT_TRUE(has("test_cfg_int"));
  EXPECT_TRUE(has("test_cfg_bool"));
  // The mode's exclusions, the store's own flags, secrets and gflags' built-ins are not.
  EXPECT_FALSE(has("test_cfg_unlisted"));
  EXPECT_FALSE(has("save_defaults"));
  EXPECT_FALSE(has("clear_defaults"));
  EXPECT_FALSE(has("test_cfg_key_password"));
  EXPECT_FALSE(has("test_cfg_cert_pem"));
  EXPECT_FALSE(has("help"));
  EXPECT_FALSE(has("flagfile"));
}

TEST_F(ConfigFlagsTest, SecretsAreNeverSaved) {
  parse({"--test_cfg_key_password=hunter2", "--test_cfg_cert_pem=PEM", "--test_cfg_cli=cli"});
  wish::persistent_store store{store_file()};
  EXPECT_EQ(wish::save_config_store_defaults(store, kEntry, {}), std::vector<std::string>{"test_cfg_cli"});
}

// ── Precedence ───────────────────────────────────────────────────────────────

TEST_F(ConfigFlagsTest, CommandLineThenEnvThenStoreThenHardcoded) {
  {
    wish::persistent_store store{store_file()};
    dynamic saved;
    saved["test_cfg_cli"_key] = std::string{"store_cli"};
    saved["test_cfg_env"_key] = std::string{"store_env"};
    saved["test_cfg_store"_key] = std::string{"store_store"};
    store.set(kEntry, saved);
  }
  set_env("WISH_TEST_CFG_CLI", "env_cli");
  set_env("WISH_TEST_CFG_ENV", "env_env");
  parse({"--test_cfg_cli=cli_cli"});

  wish::persistent_store store{store_file()};
  auto applied = wish::apply_config_store_defaults(store, kEntry, kExcluded);

  EXPECT_EQ(FLAGS_test_cfg_cli, "cli_cli");
  EXPECT_EQ(FLAGS_test_cfg_env, "env_env");
  EXPECT_EQ(FLAGS_test_cfg_store, "store_store");
  EXPECT_EQ(FLAGS_test_cfg_none, "hard_none");
  EXPECT_EQ(applied, std::vector<std::string>{"test_cfg_store"});
}

TEST_F(ConfigFlagsTest, StoreValueKeepsFlagAtDefaultAndCoercesTypes) {
  {
    wish::persistent_store store{store_file()};
    dynamic saved;
    saved["test_cfg_int"_key] = int32_t{42};
    saved["test_cfg_bool"_key] = true;
    store.set(kEntry, saved);
  }
  parse({});
  wish::persistent_store store{store_file()};
  wish::apply_config_store_defaults(store, kEntry, kExcluded);

  EXPECT_EQ(FLAGS_test_cfg_int, 42);
  EXPECT_TRUE(FLAGS_test_cfg_bool);
  // A saved value is a default, not an explicit flag (e.g. standalone's
  // transport-flag rejection only looks at explicit flags).
  EXPECT_TRUE(gflags::GetCommandLineFlagInfoOrDie("test_cfg_int").is_default);
}

TEST_F(ConfigFlagsTest, InvalidOrUnlistedStoreValuesAreIgnored) {
  {
    wish::persistent_store store{store_file()};
    dynamic saved;
    saved["test_cfg_int"_key] = std::string{"not a number"};
    saved["test_cfg_unlisted"_key] = std::string{"store_unlisted"};
    store.set(kEntry, saved);
  }
  parse({});
  wish::persistent_store store{store_file()};
  EXPECT_TRUE(wish::apply_config_store_defaults(store, kEntry, kExcluded).empty());

  EXPECT_EQ(FLAGS_test_cfg_int, 5);
  EXPECT_EQ(FLAGS_test_cfg_unlisted, "hard_unlisted");
}

TEST_F(ConfigFlagsTest, MissingEntryChangesNothing) {
  parse({});
  wish::persistent_store store{store_file()};
  EXPECT_TRUE(wish::apply_config_store_defaults(store, kEntry, kExcluded).empty());
  EXPECT_EQ(FLAGS_test_cfg_store, "hard_store");
}

// ── Saving ───────────────────────────────────────────────────────────────────

TEST_F(ConfigFlagsTest, SaveStoresOnlySaveableCommandLineFlagsTyped) {
  set_env("WISH_TEST_CFG_ENV", "env_env");
  parse({"--test_cfg_cli=cli", "--test_cfg_int=7", "--test_cfg_bool", "--test_cfg_unlisted=x"});

  wish::persistent_store store{store_file()};
  auto saved = wish::save_config_store_defaults(store, kEntry, kExcluded);
  std::sort(saved.begin(), saved.end());
  EXPECT_EQ(saved, (std::vector<std::string>{"test_cfg_bool", "test_cfg_cli", "test_cfg_int"}));

  auto entry = store.get(kEntry);
  ASSERT_TRUE(entry.has_value());
  const auto* cli = entry->findField("test_cfg_cli"_key);
  const auto* num = entry->findField("test_cfg_int"_key);
  const auto* flag = entry->findField("test_cfg_bool"_key);
  ASSERT_TRUE(cli && num && flag);
  EXPECT_TRUE(cli->is<std::string>());
  EXPECT_EQ(cli->get_as<std::string>(), "cli");
  ASSERT_TRUE(num->is<int32_t>());
  EXPECT_EQ(num->get_as<int32_t>(), 7);
  ASSERT_TRUE(flag->is<bool>());
  EXPECT_TRUE(flag->get_as<bool>());
  EXPECT_EQ(entry->findField("test_cfg_env"_key), nullptr) << "env values are not saved";
  EXPECT_EQ(entry->findField("test_cfg_unlisted"_key), nullptr);
}

TEST_F(ConfigFlagsTest, SaveMergesWithExistingEntry) {
  {
    wish::persistent_store store{store_file()};
    dynamic saved;
    saved["test_cfg_store"_key] = std::string{"kept"};
    saved["test_cfg_cli"_key] = std::string{"old"};
    store.set(kEntry, saved);
  }
  parse({"--test_cfg_cli=new"});
  wish::persistent_store store{store_file()};
  wish::save_config_store_defaults(store, kEntry, kExcluded);

  auto entry = store.get(kEntry);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->get_as<std::string>("test_cfg_cli"_key), "new");
  EXPECT_EQ(entry->get_as<std::string>("test_cfg_store"_key), "kept");
}

TEST_F(ConfigFlagsTest, SaveWithNoFlagsWritesNothing) {
  parse({});
  wish::persistent_store store{store_file()};
  EXPECT_TRUE(wish::save_config_store_defaults(store, kEntry, kExcluded).empty());
  EXPECT_FALSE(std::filesystem::exists(store_file()));
}

// ── load_cli_config ──────────────────────────────────────────────────────────

TEST_F(ConfigFlagsTest, LoadCliConfigSavesThenLaterRunUsesSavedDefaults) {
  {
    gflags::FlagSaver first_run;
    parse({"--test_cfg_store=saved", "--test_cfg_int=9", "--save_defaults"});
    wish::load_cli_config(dir_.string(), kEntry, kExcluded, "[test]");
  }
  ASSERT_TRUE(std::filesystem::exists(store_file()));
  EXPECT_EQ(FLAGS_test_cfg_store, "hard_store");

  // A later run without flags picks up the saved values...
  {
    gflags::FlagSaver second_run;
    parse({});
    wish::load_cli_config(dir_.string(), kEntry, kExcluded, "[test]");
    EXPECT_EQ(FLAGS_test_cfg_store, "saved");
    EXPECT_EQ(FLAGS_test_cfg_int, 9);
  }
  // ...and the env var and command line still win over them.
  set_env("WISH_TEST_CFG_STORE", "env");
  parse({"--test_cfg_int=1"});
  wish::load_cli_config(dir_.string(), kEntry, kExcluded, "[test]");
  EXPECT_EQ(FLAGS_test_cfg_store, "env");
  EXPECT_EQ(FLAGS_test_cfg_int, 1);
}

TEST_F(ConfigFlagsTest, LoadCliConfigClearDefaultsErasesEntry) {
  {
    wish::persistent_store store{store_file()};
    dynamic saved;
    saved["test_cfg_store"_key] = std::string{"saved"};
    store.set(kEntry, saved);
  }
  parse({"--clear_defaults"});
  wish::load_cli_config(dir_.string(), kEntry, kExcluded, "[test]");

  EXPECT_EQ(FLAGS_test_cfg_store, "hard_store");
  wish::persistent_store store{store_file()};
  EXPECT_FALSE(store.contains(kEntry));
}
