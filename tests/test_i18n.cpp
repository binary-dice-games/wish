// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <i18n/translations.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace bdg::bison;
using namespace bdg::wish;

// ── parse_translations ────────────────────────────────────────────────────────

TEST(TranslationParse, ReadsTrimmedKeyValuePairs) {
  auto map = parse_translations("HELLO = Hola mundo\n  BYE=Adios  \n");
  EXPECT_EQ(map.size(), 2u);
  EXPECT_EQ(map.lookup("HELLO"), "Hola mundo");
  EXPECT_EQ(map.lookup("BYE"), "Adios");
}

TEST(TranslationParse, SkipsCommentsBlankAndMalformedLines) {
  auto map = parse_translations("# comment\n; also comment\n\nno equals sign\n = no key\nOK = yes\n");
  EXPECT_EQ(map.size(), 1u);
  EXPECT_EQ(map.lookup("OK"), "yes");
}

TEST(TranslationParse, UnescapesValuesAndAcceptsCrlf) {
  auto map = parse_translations("A = one\\ntwo\\tthree\\\\four\r\nB = x\r\n");
  EXPECT_EQ(map.lookup("A"), "one\ntwo\tthree\\four");
  EXPECT_EQ(map.lookup("B"), "x");
}

TEST(TranslationParse, LaterDuplicateWinsAndValueMayContainEquals) {
  auto map = parse_translations("K = first\nK = a = b\n");
  EXPECT_EQ(map.lookup("K"), "a = b");
}

TEST(TranslationParse, LoadFileReturnsNulloptForMissingFile) {
  EXPECT_FALSE(load_translations_file("/nonexistent/dir/xx.lang").has_value());
}

// ── Lookup / translate_text ───────────────────────────────────────────────────

TEST(TranslationLookup, OnlyWholeValueDollarPrefixIsTranslated) {
  auto map = parse_translations("HELLO = Hola");
  EXPECT_EQ(translate_text("$$HELLO", map), "Hola");
  EXPECT_EQ(translate_text("HELLO", map), "HELLO");
  EXPECT_EQ(translate_text("say $$HELLO", map), "say $$HELLO");
  EXPECT_EQ(translate_text("", map), "");
}

TEST(TranslationLookup, MissingKeyShowsBareKey) {
  translation_map empty;
  EXPECT_EQ(translate_text("$$NOT_THERE", empty), "NOT_THERE");
}

TEST(TranslationLookup, FallsBackAlongChain) {
  auto en = std::make_shared<translation_map>(parse_translations("A = A-en\nB = B-en"));
  auto es = parse_translations("A = A-es");
  es.set_fallback(en);
  EXPECT_EQ(es.lookup("A"), "A-es");
  EXPECT_EQ(es.lookup("B"), "B-en");
  EXPECT_EQ(es.lookup("C"), "C");
  EXPECT_EQ(es.find("C"), nullptr);
}

TEST(TranslationLookup, ValidLanguageCodes) {
  EXPECT_TRUE(is_valid_language_code(""));
  EXPECT_TRUE(is_valid_language_code("es"));
  EXPECT_TRUE(is_valid_language_code("pt-BR"));
  EXPECT_TRUE(is_valid_language_code("zh_Hans"));
  EXPECT_FALSE(is_valid_language_code("../etc"));
  EXPECT_FALSE(is_valid_language_code("e s"));
  EXPECT_FALSE(is_valid_language_code("es/x"));
}

// ── Descriptor import with translations ───────────────────────────────────────

namespace {

const translation_map& sample_map() {
  // Translations with characters that would break JSON/YAML if substituted
  // into the text before parsing.
  static const translation_map map = parse_translations(
      "TITLE = Ventana \"principal\": {1}\n"
      "OK = Aceptar\n");
  return map;
}

dynamic_ptr child(const dynamic& node, const char* name) {
  auto children = node.findField("children"_key)->as<dynamic_ptr>();
  return (*children)[bdg::bison::key_t{name}].as<dynamic_ptr>();
}

} // namespace

TEST(TranslationImport, JsonStringFieldsAreTranslatedAtEveryDepth) {
  auto desc = import_descriptor_json(
      R"({"type":"Window","title":"$$TITLE","width":300,
          "children":{"row":{"type":"HorizontalLayout","children":{
            "ok":{"type":"Button","label":"$$OK"},
            "plain":{"type":"Label","text":"not a key $$OK"}}}}})",
      &sample_map());
  EXPECT_EQ(desc.as<std::string>("title"_key), "Ventana \"principal\": {1}");
  EXPECT_EQ(desc.findField("width"_key)->get_as<int32_t>(), 300);
  auto row = child(desc, "row");
  EXPECT_EQ(child(*row, "ok")->as<std::string>("label"_key), "Aceptar");
  EXPECT_EQ(child(*row, "plain")->as<std::string>("text"_key), "not a key $$OK");
}

TEST(TranslationImport, YamlStringFieldsAreTranslated) {
  auto desc = import_descriptor_yaml(
      "type: Window\n"
      "title: $$TITLE\n"
      "children:\n"
      "  - type: Button\n"
      "    label: $$OK\n",
      &sample_map());
  EXPECT_EQ(desc.as<std::string>("title"_key), "Ventana \"principal\": {1}");
  auto children = desc.findField("children"_key)->as<dynamic_ptr>();
  EXPECT_EQ((*children)[size_t{0}].as<dynamic_ptr>()->as<std::string>("label"_key), "Aceptar");
}

TEST(TranslationImport, WithoutMapKeysAreLeftUntouched) {
  auto desc = import_descriptor_text(R"({"type":"Label","text":"$$OK"})");
  EXPECT_EQ(desc.as<std::string>("text"_key), "$$OK");
}

// ── context::translations_for ─────────────────────────────────────────────────

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << content;
}

} // namespace

class ContextTranslationsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ctx_ = std::make_unique<context>(rmi::shared::generate_id());
    auto dir = ctx_->resource_dir / "res" / "test" / "i18n" / "mod" / "i18n";
    write_file(dir / "en.lang", "A = A-en\nB = B-en\n");
    write_file(dir / "es.lang", "A = A-es\n");
  }

  std::unique_ptr<context> ctx_;
};

TEST_F(ContextTranslationsTest, DefaultLanguageLoadsEnglish) {
  auto map = ctx_->translations_for("test/i18n/mod");
  EXPECT_EQ(map->lookup("A"), "A-en");
  EXPECT_EQ(map->lookup("B"), "B-en");
}

TEST_F(ContextTranslationsTest, SelectedLanguageFallsBackToEnglish) {
  ctx_->set_language("es");
  auto map = ctx_->translations_for("test/i18n/mod");
  EXPECT_EQ(map->lookup("A"), "A-es");
  EXPECT_EQ(map->lookup("B"), "B-en");
}

TEST_F(ContextTranslationsTest, ChangingLanguageReloadsButKeepsOldMapValid) {
  auto en = ctx_->translations_for("test/i18n/mod");
  ctx_->set_language("es");
  auto es = ctx_->translations_for("test/i18n/mod");
  EXPECT_EQ(en->lookup("A"), "A-en");
  EXPECT_EQ(es->lookup("A"), "A-es");
}

TEST_F(ContextTranslationsTest, MissingLanguageFileUsesEnglish) {
  ctx_->set_language("fr");
  EXPECT_EQ(ctx_->translations_for("test/i18n/mod")->lookup("A"), "A-en");
}

TEST_F(ContextTranslationsTest, MissingModuleYieldsEmptyMap) {
  auto map = ctx_->translations_for("no/such/module");
  ASSERT_NE(map, nullptr);
  EXPECT_TRUE(map->empty());
}

TEST_F(ContextTranslationsTest, InvalidLanguageCodeIsRejected) {
  EXPECT_THROW(ctx_->set_language("../../etc"), std::invalid_argument);
  EXPECT_EQ(ctx_->language(), "");
}

TEST_F(ContextTranslationsTest, BuiltInFormTranslationsAreEmbedded) {
  // resources/embedded/i18n/ ships with the server's embedded resources.
  ctx_->set_language("es");
  auto map = ctx_->translations_for("");
  EXPECT_EQ(map->lookup("MSGBOX_CANCEL"), "Cancelar");
}
