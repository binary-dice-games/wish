// MIT License © 2025 Binary Dice Games
//
// Guards the checked-in editor example UIs (examples/ui/json/*.json and
// examples/ui/yaml/*.yaml): every file must import cleanly through the same
// importer the `editor` module uses, and each JSON file must have a YAML
// twin (same basename) that parses to the same number of named nodes.
#include <gtest/gtest.h>

#include <server/registry.hpp>
#include <ui/ui_importer.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#ifndef WISH_EXAMPLES_UI_DIR
#error "WISH_EXAMPLES_UI_DIR must be defined by the build (path to examples/ui)"
#endif

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

std::map<std::string, fs::path> files_with_extension(const fs::path& dir, const std::string& ext) {
  std::map<std::string, fs::path> out;
  for (const auto& e : fs::directory_iterator(dir))
    if (e.is_regular_file() && e.path().extension() == ext)
      out.emplace(e.path().stem().string(), e.path());
  return out;
}

} // namespace

class ExamplesUiTest : public ::testing::Test {
 protected:
  void SetUp() override { bdg::wish::register_all(); }

  fs::path json_dir() const { return fs::path{WISH_EXAMPLES_UI_DIR} / "json"; }
  fs::path yaml_dir() const { return fs::path{WISH_EXAMPLES_UI_DIR} / "yaml"; }
};

TEST_F(ExamplesUiTest, JsonAndYamlDirsExistAndAreNonEmpty) {
  ASSERT_TRUE(fs::is_directory(json_dir())) << json_dir();
  ASSERT_TRUE(fs::is_directory(yaml_dir())) << yaml_dir();
  EXPECT_FALSE(files_with_extension(json_dir(), ".json").empty());
  EXPECT_FALSE(files_with_extension(yaml_dir(), ".yaml").empty());
}

TEST_F(ExamplesUiTest, EveryJsonExampleImports) {
  for (const auto& [stem, path] : files_with_extension(json_dir(), ".json")) {
    auto tree = bdg::wish::import_json(read_file(path));
    EXPECT_FALSE(tree.empty()) << stem << ".json produced an empty tree";
    EXPECT_TRUE(tree.count("")) << stem << ".json has no root node";
  }
}

TEST_F(ExamplesUiTest, EveryYamlExampleImports) {
  for (const auto& [stem, path] : files_with_extension(yaml_dir(), ".yaml")) {
    auto tree = bdg::wish::import_yaml(read_file(path));
    EXPECT_FALSE(tree.empty()) << stem << ".yaml produced an empty tree";
    EXPECT_TRUE(tree.count("")) << stem << ".yaml has no root node";
  }
}

TEST_F(ExamplesUiTest, JsonAndYamlExamplesPairUpAndMatch) {
  auto jsons = files_with_extension(json_dir(), ".json");
  auto yamls = files_with_extension(yaml_dir(), ".yaml");
  ASSERT_EQ(jsons.size(), yamls.size());

  for (const auto& [stem, jpath] : jsons) {
    auto it = yamls.find(stem);
    ASSERT_NE(it, yamls.end()) << "no YAML twin for " << stem << ".json";
    auto jtree = bdg::wish::import_json(read_file(jpath));
    auto ytree = bdg::wish::import_yaml(read_file(it->second));
    EXPECT_EQ(jtree.size(), ytree.size()) << stem << ": JSON/YAML node counts differ";
  }
}
