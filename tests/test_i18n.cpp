// MIT License © 2025 Binary Dice Games
#include <gtest/gtest.h>

#include <context/context.hpp>
#include <i18n/translations.hpp>
#include <ui/ui_descriptor.hpp>

#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/ids.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
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

// ── format_text ───────────────────────────────────────────────────────────────

TEST(TranslationFormat, ReplacesNumberedPlaceholders) {
  EXPECT_EQ(format_text("{0} of {1}", {"3", "7"}), "3 of 7");
  EXPECT_EQ(format_text("{1} before {0}", {"a", "b"}), "b before a");
  EXPECT_EQ(format_text("{2} {x} {} {0}", {"a"}), "{2} {x} {} a");
}

// ── Lookup / translate_text ───────────────────────────────────────────────────

TEST(TranslationLookup, OnlyWholeValueDollarPrefixIsTranslated) {
  auto map = parse_translations("HELLO = Hola");
  EXPECT_EQ(translate_text("$$HELLO", map), "Hola");
  EXPECT_EQ(translate_text("HELLO", map), "HELLO");
  EXPECT_EQ(translate_text("say $$HELLO", map), "say $$HELLO");
  EXPECT_EQ(translate_text("", map), "");
}

TEST(TranslationLookup, MessageArgumentsAreFormatted) {
  auto map = parse_translations("SCANNED = {0} elementos ({1})");
  EXPECT_EQ(translate_text(make_message("$$SCANNED", {"5", "3 MB"}), map), "5 elementos (3 MB)");
  EXPECT_EQ(translate_text(make_message("$$SCANNED", {""}), map), " elementos ({1})");
  EXPECT_EQ(translate_text(make_message("$$MISSING", {"x"}), map), "MISSING");
  auto nested = parse_translations("FAILED = {0} falló\nPULL = descarga");
  EXPECT_EQ(translate_text(make_message("$$FAILED", {"$$PULL"}), nested), "descarga falló");
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

TEST_F(ContextTranslationsTest, MissingModuleFallsBackToBuiltIn) {
  auto map = ctx_->translations_for("no/such/module");
  ASSERT_NE(map, nullptr);
  EXPECT_EQ(map->lookup("UNKNOWN_KEY"), "UNKNOWN_KEY");
  EXPECT_EQ(map->lookup("COMMON_REFRESH"), "&Refresh");
}

TEST_F(ContextTranslationsTest, ModuleChainEndsInBuiltInTranslations) {
  ctx_->set_language("fr");
  auto map = ctx_->translations_for("test/i18n/mod");
  EXPECT_EQ(map->lookup("A"), "A-en");  // no fr file for the module
  EXPECT_EQ(map->lookup("COMMON_REFRESH"), "&Actualiser");
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

// ── Shipped translation files ─────────────────────────────────────────────────
//
// Every translation directory (built-in res/i18n and each module's) must ship
// the same keys in every language, with the same {N} placeholders, and every
// "$$KEY" a module's code uses must be defined -- so a missing translation
// fails here instead of showing a bare key in the UI.

namespace {

namespace fs = std::filesystem;

const char* const kShippedLanguages[] = {"en", "es", "fr"};

std::set<std::string> placeholders(const std::string& text) {
  std::set<std::string> out;
  static const std::regex re{R"(\{[0-9]\})"};
  for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
    out.insert(it->str());
  return out;
}

std::map<std::string, std::string> read_lang(const fs::path& path) {
  std::map<std::string, std::string> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#' || line[first] == ';')
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    auto trim = [](std::string s) {
      s.erase(0, s.find_first_not_of(" \t"));
      s.erase(s.find_last_not_of(" \t") + 1);
      return s;
    };
    out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
  }
  return out;
}

std::vector<fs::path> translation_dirs() {
  const fs::path root{WISH_SOURCE_DIR};
  std::vector<fs::path> dirs{root / "resources" / "embedded" / "i18n"};
  for (const auto& entry : fs::recursive_directory_iterator(root / "modules")) {
    if (entry.is_directory() && entry.path().filename() == "i18n" &&
        entry.path().parent_path().filename() == "embedded")
      dirs.push_back(entry.path());
  }
  return dirs;
}

std::set<std::string> keys_used_in(const fs::path& dir) {
  std::set<std::string> keys;
  // Not preceded by a backslash: a \"$$KEY\" inside a string literal is a
  // documentation example (e.g. a field's Description attribute).
  static const std::regex re{R"re((?:^|[^\\])"\$\$([A-Z][A-Z0-9_]*))re"};
  for (const auto& entry : fs::recursive_directory_iterator(dir)) {
    const auto ext = entry.path().extension();
    if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".hpp"))
      continue;
    std::ifstream in(entry.path());
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
      keys.insert((*it)[1].str());
  }
  return keys;
}

} // namespace

TEST(ShippedTranslations, EveryLanguageHasTheSameKeysAndPlaceholders) {
  const auto dirs = translation_dirs();
  ASSERT_GT(dirs.size(), 10u);
  for (const auto& dir : dirs) {
    const auto en = read_lang(dir / "en.lang");
    ASSERT_FALSE(en.empty()) << dir;
    for (const char* lang : kShippedLanguages) {
      const auto other = read_lang(dir / (std::string{lang} + ".lang"));
      for (const auto& [key, text] : en) {
        auto it = other.find(key);
        if (it == other.end()) {
          ADD_FAILURE() << dir << ": " << lang << ".lang is missing " << key;
          continue;
        }
        EXPECT_EQ(placeholders(it->second), placeholders(text)) << dir << ": " << lang << " " << key;
      }
      for (const auto& [key, text] : other)
        EXPECT_TRUE(en.count(key)) << dir << ": " << lang << ".lang has extra key " << key;
    }
  }
}

TEST(ShippedTranslations, LabelsMarkAtMostOneAccessKey) {
  // "&X" marks a control's access key (see bdg::wish::parse_access_key());
  // only the first counts, so a second one would show its '&' literally.
  auto markers = [](const std::string& text) {
    int n = 0;
    for (size_t i = 0; i + 1 < text.size(); ++i) {
      if (text[i] == '&' && text[i + 1] == '&')
        ++i;
      else if (text[i] == '&' && std::isalnum(static_cast<unsigned char>(text[i + 1])))
        ++n;
    }
    return n;
  };
  for (const auto& dir : translation_dirs())
    for (const char* lang : kShippedLanguages)
      for (const auto& [key, text] : read_lang(dir / (std::string{lang} + ".lang")))
        EXPECT_LE(markers(text), 1) << dir << ": " << lang << " " << key << " = " << text;
}

TEST(ShippedTranslations, EveryKeyAModuleUsesIsDefined) {
  const fs::path root{WISH_SOURCE_DIR};
  const auto builtin = read_lang(root / "resources" / "embedded" / "i18n" / "en.lang");
  for (const auto& dir : translation_dirs()) {
    if (dir.parent_path().filename() != "embedded" || dir.parent_path().parent_path().filename() != "resources")
      continue;
    const fs::path module = dir.parent_path().parent_path().parent_path();
    if (module == root)
      continue;
    const auto own = read_lang(dir / "en.lang");
    for (const auto& key : keys_used_in(module))
      EXPECT_TRUE(own.count(key) || builtin.count(key)) << module << " uses undefined key " << key;
  }
}

TEST(ShippedTranslations, EmbeddedModuleTranslationsResolveInEveryLanguage) {
  // Built modules ship their i18n files in the embedded resources; each
  // session resolves every key to that language's text.
  const fs::path root{WISH_SOURCE_DIR};
  size_t checked = 0;
  for (const char* lang : kShippedLanguages) {
    context ctx{rmi::shared::generate_id()};
    ctx.set_language(lang);
    for (const auto& dir : translation_dirs()) {
      const fs::path module = dir.parent_path().parent_path().parent_path();
      const std::string prefix =
          module == root ? std::string{} : fs::relative(module, root / "modules").generic_string();
      const fs::path embedded = ctx.resource_dir / "res" / (prefix.empty() ? fs::path{} : fs::path{prefix}) / "i18n" /
          (std::string{lang} + ".lang");
      if (!fs::exists(embedded))
        continue; // module not built into this binary
      const auto expected = read_lang(dir / (std::string{lang} + ".lang"));
      const auto map = ctx.translations_for(prefix);
      for (const auto& [key, text] : expected) {
        // The map unescapes \n and friends; compare those values unescaped too.
        EXPECT_EQ(map->lookup(key), parse_translations(key + " = " + text).lookup(key))
            << prefix << " " << lang << " " << key;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 3u);
}
