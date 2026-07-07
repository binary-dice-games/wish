// MIT License © 2025 Binary Dice Games
//
// Proves the wish_client / wish_server CMake split actually works for a
// real thin client: this translation unit links ONLY the wish_client
// target (see tests/CMakeLists.txt) -- no wish_server, no UI element
// registry, no renderer -- yet can still parse a JSON descriptor into a
// bison::dynamic tree and build one by hand, exactly what a genuine remote
// client needs to call client::register_template without ever linking the
// heavy server stack.
#include <gtest/gtest.h>

#include <client.hpp>
#include <ui_descriptor.hpp>

using namespace bdg::bison;
namespace bison = bdg::bison;
namespace wish = bdg::wish;

TEST(ClientOnlyLinkTest, ImportDescriptorJsonWorksWithoutServerLinked) {
  auto desc = wish::import_descriptor_json(R"({
    "type": "Window",
    "title": "Hello",
    "children": { "ok": { "type": "Button", "label": "OK" } }
  })");

  EXPECT_TRUE(desc.findField("__type__"_key)->is<bison::key_t>());

  auto children = desc.findField("children"_key)->as<dynamic_ptr>();
  ASSERT_NE(children, nullptr);
  auto ok = (*children)[bison::key_t{"ok"}].as<dynamic_ptr>();
  ASSERT_NE(ok, nullptr);
  EXPECT_EQ(ok->findField("__name__"_key)->as<std::string>(), "ok");
  EXPECT_EQ(ok->findField("label"_key)->as<std::string>(), "OK");
}

TEST(ClientOnlyLinkTest, HandBuiltDescriptorSkipsTextEntirely) {
  // A caller with no JSON/YAML at all can build the same shape directly.
  dynamic desc;
  desc["__type__"_key] = "Window"_key;
  desc["title"_key] = std::string{"Hello"};

  EXPECT_EQ(desc.findField("__type__"_key)->as<bison::key_t>(), "Window"_key);
  EXPECT_EQ(desc.findField("title"_key)->as<std::string>(), "Hello");
}

TEST(ClientOnlyLinkTest, ClientRegisterTemplateSymbolResolvesWithoutWishServer) {
  // Not connected to any transport -- this only proves the symbol for
  // client::register_template(key_t, dynamic) links from wish_client alone.
  using register_template_fn = std::future<void> (wish::client::*)(bison::key_t, dynamic);
  register_template_fn fn = &wish::client::register_template;
  EXPECT_NE(fn, nullptr);
}
