// MIT License © 2025 Binary Dice Games
/// @file style_service.cpp
/// @brief Per-session style/theme RMI service implementation.
#include <context/style_service.hpp>

#include "src/bison/bison_sync.hpp"

#include <cctype>
#include <cstdint>

#include <string>

namespace bdg::wish {

using namespace bison;

// ── helpers ───────────────────────────────────────────────────────────────────

// Copy all float and string fields from src into dst (merge, not replace).
static void merge_fields(const bison::dynamic& src, bison::dynamic& dst) {
  // const_cast: forEach is non-const in bison; we are only reading src.
  const_cast<bison::dynamic&>(src).forEach([&dst](key_t k, const field& f) {
    if (f.is<std::string>())
      dst[k] = f.as<std::string>();
    else if (f.is<float>())
      dst[k] = f.as<float>();
    else if (f.is<int32_t>())
      dst[k] = f.as<int32_t>();
    else if (f.is<bool>())
      dst[k] = f.as<bool>();
  });
}

// ── style_service ─────────────────────────────────────────────────────────────

style_service_ptr style_service::instantiate() {
    return std::make_shared<style_service>(bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishStyle"}));
}

style_service::style_service(bison::dynamic&& base) : dynamic(std::move(base)) {}

void style_service::set_fields(const bison::dynamic& params) {
  merge_fields(params, style_);
  dirty_.store(true, std::memory_order_release);
}

bison::dynamic style_service::get_fields() const {
  bison::dynamic result;
  merge_fields(style_, result);
  return result;
}

void style_service::set_preset(const std::string& name) {
  // Clear all overrides and record only the preset name, unvalidated. The
  // renderer looks up and applies the matching registered theme function
  // (see imgui_renderer::register_theme()) -- and records is_light_theme()
  // -- on the render thread, falling back to the default theme with a
  // logged warning if @p name isn't registered there.
  style_ = bison::dynamic{};
  style_["preset"_key] = name;
  dirty_.store(true, std::memory_order_release);
}

// ── registration ──────────────────────────────────────────────────────────────

void register_style_service() {
  auto proto = dynamic_ptr{"__WishStyle"_key, {}};
  proto->addMethod(
      "set"_key,
      bison::method{
          [](dynamic& s, const dynamic& p) -> dynamic {
            static_cast<style_service&>(s).set_fields(p);
            return dynamic{};
          },
          attr<DisplayName>("set")});
  proto->addMethod(
      "get"_key,
      bison::method{
          [](dynamic& s, const dynamic& /*p*/) -> dynamic { return static_cast<style_service&>(s).get_fields(); },
          attr<DisplayName>("get")});
  auto preset_in = std::make_shared<dynamic>();
  preset_in->addField("name"_key, field{std::string{}, attr<DisplayName>("name")});
  proto->addMethod(
      "preset"_key,
      bison::method{
          [](dynamic& s, const dynamic& p) -> dynamic {
            static_cast<style_service&>(s).set_preset(p.as<std::string>("name"_key));
            return dynamic{};
          },
          dynamic_ptr{preset_in},
          nullptr,
          attr<DisplayName>("preset")});
  proto->addMethod(
      "themes"_key,
      bison::method{
          [](dynamic& /*s*/, const dynamic& /*p*/) -> dynamic {
            dynamic list;
            uint32_t i = 0;
            for (const auto& t : registered_themes()) {
              auto item = std::make_shared<dynamic>();
              (*item)["name"_key] = t.name;
              (*item)["label"_key] = t.label;
              (*item)["light"_key] = t.is_light;
              list[key_t{i++}] = dynamic_ptr{item};
            }
            return list;
          },
          attr<DisplayName>("themes")});
  dynamic::addClass("wish"_key, std::move(proto));
}

// ── Theme catalog ─────────────────────────────────────────────────────────────

namespace {

bison::synchronized<std::vector<theme_info>>& theme_catalog() {
  static bison::synchronized<std::vector<theme_info>> catalog;
  return catalog;
}

} // namespace

std::string default_theme_label(const std::string& name) {
  std::string label = name;
  if (!label.empty())
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
  return label;
}

void add_theme_info(theme_info info) {
  if (info.label.empty())
    info.label = default_theme_label(info.name);
  auto catalog = theme_catalog().wlock();
  for (auto& t : *catalog) {
    if (t.name == info.name) {
      t = std::move(info);
      return;
    }
  }
  catalog->push_back(std::move(info));
}

std::vector<theme_info> registered_themes() {
  return *theme_catalog().rlock();
}

std::optional<theme_info> find_theme_info(const std::string& name) {
  for (const auto& t : *theme_catalog().rlock()) {
    if (t.name == name)
      return t;
  }
  return std::nullopt;
}

std::string next_theme(const std::string& name) {
  const auto themes = registered_themes();
  if (themes.empty())
    return {};
  for (size_t i = 0; i < themes.size(); ++i) {
    if (themes[i].name == name)
      return themes[(i + 1) % themes.size()].name;
  }
  return themes.front().name;
}

} // namespace bdg::wish
