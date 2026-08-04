// MIT License © 2025 Binary Dice Games
/// @file automation_service.cpp
/// @brief Implementation of bdg::wish::automation_service.
#include <automation/automation_service.hpp>

#ifdef WISH_AUTOMATION_ENABLED

#include <automation/automation_query.hpp>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace bdg::wish {

using namespace bison;

// ── automation_service ───────────────────────────────────────────────────────

automation_service_ptr automation_service::instantiate(automation::automation_backend* backend, logger_ptr logger) {
  return std::make_shared<automation_service>(
      bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishAutomation"}), backend,
      std::move(logger));
}

automation_service::automation_service(dynamic&& base, automation::automation_backend* backend, logger_ptr logger)
    : dynamic(std::move(base)), backend_(backend), logger_(std::move(logger)) {
  addMethod(
      "get_tree"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        std::string root;
        if (const auto* f = p.findField("root"_key); f && f->is<std::string>())
          root = f->as<std::string>();
        auto json = backend_->query_tree(next_request_id_.fetch_add(1, std::memory_order_relaxed), root).get();
        dynamic result;
        result["json"_key] = std::move(json);
        return result;
      }});

  addMethod(
      "get_logs"_key, bison::method{[this](dynamic& /*self*/, const dynamic& /*p*/) -> dynamic {
        std::deque<logger::log_entry> entries;
        if (logger_)
          entries = logger_->recent_logs();
        dynamic result;
        result["json"_key] = automation::build_log_event(entries);
        return result;
      }});

  addMethod(
      "screenshot"_key, bison::method{[this](dynamic& /*self*/, const dynamic& /*p*/) -> dynamic {
        auto bytes = backend_->capture_screenshot().get();
        dynamic result;
        result["data"_key] = std::string(bytes.begin(), bytes.end());
        return result;
      }});

  addMethod(
      "mouse_move"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        backend_->inject_mouse_move(p.as<float>("x"_key), p.as<float>("y"_key));
        return dynamic{};
      }});

  addMethod(
      "mouse_button"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        backend_->inject_mouse_button(p.as<int32_t>("button"_key), p.as<bool>("down"_key));
        return dynamic{};
      }});

  addMethod(
      "key_event"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        backend_->inject_key(p.as<int32_t>("keycode"_key), p.as<bool>("down"_key));
        return dynamic{};
      }});

  addMethod(
      "text_input"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        backend_->inject_text(p.as<std::string>("utf8"_key));
        return dynamic{};
      }});
}

// ── registration ──────────────────────────────────────────────────────────────

void register_automation_service() {
  auto proto = dynamic_ptr{"__WishAutomation"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish

#endif // WISH_AUTOMATION_ENABLED
