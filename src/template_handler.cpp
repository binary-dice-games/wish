// MIT License © 2025 Binary Dice Games
/// @file template_handler.cpp
/// @brief Implementation of the server-side __WishTemplate RMI class.
#include "template_handler.hpp"

#include "import_handler.hpp"

#include <stdexcept>

namespace bdg::wish {

using namespace bison;

// ── template_handler ─────────────────────────────────────────────────────────

void template_handler::register_class() {
  auto proto = dynamic_ptr{"__WishTemplate"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

template_handler::template_handler(
    bison::dynamic&& base,
    bison::rmi::context& ctx,
    std::shared_ptr<session> sess)
    : dynamic(std::move(base)), ctx_(ctx), sess_(std::move(sess)) {
  addMethod(
      "register"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        bison::key_t name = params.as<bison::key_t>("name"_key);
        std::string descriptor = params.as<std::string>("descriptor"_key);
        sess_->templates[name] = std::move(descriptor);
        return dynamic{};
      }});

  addMethod(
      "instantiate"_key,
      bison::method{[this](dynamic& /*self*/,
                            const dynamic& params) -> dynamic {
        bison::key_t name = params.as<bison::key_t>("name"_key);
        auto it = sess_->templates.find(name);
        if (it == sess_->templates.end()) {
          throw std::runtime_error(
              "wish: template not found");
        }
        return apply_descriptor(ctx_, *sess_, it->second);
      }});
}

}  // namespace bdg::wish
