// MIT License © 2025 Binary Dice Games
/// @file style_service.cpp
/// @brief Per-session style/theme RMI service implementation.
#include <style_service.hpp>

#include <stdexcept>
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
  if (name != "dark" && name != "light" && name != "classic") {
    throw std::runtime_error(
        "wish::style_service: unknown preset '" + name + "'; expected 'dark', 'light', or 'classic'");
  }
  // Clear all overrides and record only the preset name.
  // The renderer applies ImGui::StyleColors<Preset>() on the render thread.
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
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish
