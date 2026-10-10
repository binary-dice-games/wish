// MIT License © 2025 Binary Dice Games
//
// Unit tests for wish::persistent_store and wish::store_registry (see
// docs/persistent-store.md): CRUD, persistence across instances, nested
// objects, corrupt-file recovery, name validation, and registry sharing.
#include <gtest/gtest.h>

#include <context/persistent_store.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace bdg::bison;
namespace wish = bdg::wish;

namespace {

class PersistentStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() / (std::string{"wish_store_test_"} + info->name());
    std::filesystem::remove_all(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path file() const {
    return dir_ / "store.bison";
  }

  std::filesystem::path dir_;
};

dynamic make_settings(const std::string& filter, int32_t lines) {
  dynamic d;
  d["filter"_key] = filter;
  d["lines"_key] = lines;
  return d;
}

} // namespace

// ── Basic operations ─────────────────────────────────────────────────────────

TEST_F(PersistentStoreTest, MissingFileIsEmptyAndCreatesNothing) {
  wish::persistent_store store{file()};
  EXPECT_TRUE(store.keys().empty());
  EXPECT_FALSE(store.get("anything").has_value());
  EXPECT_FALSE(std::filesystem::exists(dir_)) << "opening a store must not create files";
}

TEST_F(PersistentStoreTest, SetGetEraseRoundTrip) {
  wish::persistent_store store{file()};
  store.set("bdg.desktop.tail", make_settings("error|warn", 200));

  ASSERT_TRUE(store.contains("bdg.desktop.tail"));
  auto value = store.get("bdg.desktop.tail");
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(value->as<std::string>("filter"_key), "error|warn");
  EXPECT_EQ(value->as<int32_t>("lines"_key), 200);

  EXPECT_TRUE(store.erase("bdg.desktop.tail"));
  EXPECT_FALSE(store.erase("bdg.desktop.tail"));
  EXPECT_FALSE(store.get("bdg.desktop.tail").has_value());
}

TEST_F(PersistentStoreTest, SetReplacesExistingEntry) {
  wish::persistent_store store{file()};
  store.set("a", make_settings("one", 1));
  store.set("a", make_settings("two", 2));
  EXPECT_EQ(store.get("a")->as<std::string>("filter"_key), "two");
  EXPECT_EQ(store.keys().size(), 1u);
}

TEST_F(PersistentStoreTest, KeysAreSorted) {
  wish::persistent_store store{file()};
  store.set("zeta", dynamic{});
  store.set("alpha", dynamic{});
  store.set("mid", dynamic{});
  EXPECT_EQ(store.keys(), (std::vector<std::string>{"alpha", "mid", "zeta"}));
}

TEST_F(PersistentStoreTest, ReturnedValueIsACopy) {
  wish::persistent_store store{file()};
  store.set("a", make_settings("orig", 1));
  auto value = store.get("a");
  (*value)["filter"_key] = std::string{"changed"};
  EXPECT_EQ(store.get("a")->as<std::string>("filter"_key), "orig");
}

// ── Persistence ──────────────────────────────────────────────────────────────

TEST_F(PersistentStoreTest, EntriesSurviveReopen) {
  {
    wish::persistent_store store{file()};
    store.set("first", make_settings("x", 1));
    store.set("second", make_settings("y", 2));
    store.erase("first");
  }
  wish::persistent_store reopened{file()};
  EXPECT_EQ(reopened.keys(), (std::vector<std::string>{"second"}));
  EXPECT_EQ(reopened.get("second")->as<std::string>("filter"_key), "y");
  EXPECT_FALSE(std::filesystem::exists(file().string() + ".tmp"));
}

TEST_F(PersistentStoreTest, NestedObjectsAndTypedFieldsRoundTrip) {
  {
    dynamic inner;
    inner["enabled"_key] = true;
    inner["ratio"_key] = 0.5f;
    inner["kind"_key] = bdg::bison::key_t{"some_hashed_name"};
    inner["data"_key] = std::vector<int32_t>{1, 2, 3};
    dynamic value;
    value["inner"_key] = dynamic_ptr{std::make_shared<dynamic>(inner)};
    wish::persistent_store store{file()};
    store.set("nested", value);
  }
  wish::persistent_store reopened{file()};
  auto value = reopened.get("nested");
  ASSERT_TRUE(value.has_value());
  const auto& inner = value->as<dynamic_ptr>("inner"_key);
  ASSERT_TRUE(inner);
  EXPECT_TRUE(inner->as<bool>("enabled"_key));
  EXPECT_FLOAT_EQ(inner->as<float>("ratio"_key), 0.5f);
  EXPECT_EQ(inner->as<bdg::bison::key_t>("kind"_key), bdg::bison::key_t{"some_hashed_name"});
  EXPECT_EQ(inner->as<std::vector<int32_t>>("data"_key), (std::vector<int32_t>{1, 2, 3}));
}

TEST_F(PersistentStoreTest, CorruptFileStartsEmptyAndIsKeptAsBackup) {
  std::filesystem::create_directories(dir_);
  {
    std::ofstream out(file(), std::ios::binary);
    out << "this is not a store";
  }
  wish::persistent_store store{file()};
  EXPECT_TRUE(store.keys().empty());
  EXPECT_TRUE(std::filesystem::exists(file().string() + ".corrupt"));

  // The store stays usable after recovery.
  store.set("a", make_settings("ok", 1));
  wish::persistent_store reopened{file()};
  EXPECT_TRUE(reopened.contains("a"));
}

TEST_F(PersistentStoreTest, TruncatedFileIsTreatedAsCorrupt) {
  {
    wish::persistent_store store{file()};
    store.set("a", make_settings("some longer filter text", 1));
  }
  const auto size = std::filesystem::file_size(file());
  std::filesystem::resize_file(file(), size - 4);
  wish::persistent_store reopened{file()};
  EXPECT_TRUE(reopened.keys().empty());
  EXPECT_TRUE(std::filesystem::exists(file().string() + ".corrupt"));
}

// ── Name validation ──────────────────────────────────────────────────────────

TEST_F(PersistentStoreTest, InvalidNamesAreRejected) {
  wish::persistent_store store{file()};
  EXPECT_THROW(store.set("", dynamic{}), std::invalid_argument);
  EXPECT_THROW(
      store.set(std::string(wish::persistent_store::kMaxNameLength + 1, 'a'), dynamic{}), std::invalid_argument);
  EXPECT_THROW(store.get("bad\nname"), std::invalid_argument);
  EXPECT_THROW(store.erase(std::string{"\x01"}), std::invalid_argument);
  EXPECT_NO_THROW(store.set(std::string(wish::persistent_store::kMaxNameLength, 'a'), dynamic{}));
  EXPECT_NO_THROW(store.set("with spaces/and.dots-1", dynamic{}));
}

// ── store_registry ───────────────────────────────────────────────────────────

TEST_F(PersistentStoreTest, RegistryUsesExpectedFileLayout) {
  wish::store_registry registry{dir_};
  registry.server_store()->set("s", dynamic{});
  registry.user_store("alice")->set("u", dynamic{});
  EXPECT_TRUE(std::filesystem::exists(dir_ / "server_store.bison"));
  EXPECT_TRUE(std::filesystem::exists(dir_ / "users" / "alice.bison"));
}

TEST_F(PersistentStoreTest, RegistrySharesStoresPerIdentity) {
  wish::store_registry registry{dir_};
  EXPECT_EQ(registry.server_store(), registry.server_store());
  auto alice1 = registry.user_store("alice");
  auto alice2 = registry.user_store("alice");
  auto bob = registry.user_store("bob");
  EXPECT_EQ(alice1, alice2);
  EXPECT_NE(alice1, bob);
}

TEST_F(PersistentStoreTest, RegistryRejectsAnonymousAndUnsafeIdentities) {
  wish::store_registry registry{dir_};
  EXPECT_EQ(registry.user_store(""), nullptr);
  EXPECT_EQ(registry.user_store("../evil"), nullptr);
  EXPECT_EQ(registry.user_store("a/b"), nullptr);
  EXPECT_EQ(registry.user_store("a\\b"), nullptr);
}

TEST(DefaultStoreDir, IsDotWishUnderHome) {
  EXPECT_EQ(wish::default_store_dir().filename(), ".wish");
}
