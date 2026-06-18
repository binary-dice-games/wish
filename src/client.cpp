// MIT License © 2025 Binary Dice Games
/// @file client.cpp
/// @brief Implementation of wish::client.
#include <wish/client.hpp>

#include <stdexcept>

namespace bdg::wish {

// ── client ────────────────────────────────────────────────────────────────────

void client::run() {
  connect();
  try {
    on_session();
  } catch (...) {
    disconnect();
    throw;
  }
  disconnect();
}

std::future<proxy_map> client::import_ui(const std::string& /*descriptor*/) {
  // Full implementation in Step 11 (requires server-side __WishImport handler).
  throw std::logic_error("wish::client::import_ui: not yet implemented");
}

std::future<void> client::register_template(
    bison::key_t /*name*/, const std::string& /*descriptor*/) {
  throw std::logic_error(
      "wish::client::register_template: not yet implemented");
}

std::future<proxy_map> client::instantiate_template(
    bison::key_t /*name*/) {
  throw std::logic_error(
      "wish::client::instantiate_template: not yet implemented");
}

std::future<void> client::upload_file(
    const std::string& /*name*/, const std::string& /*data*/) {
  throw std::logic_error("wish::client::upload_file: not yet implemented");
}

std::future<std::string> client::download_file(
    const std::string& /*name*/) {
  throw std::logic_error("wish::client::download_file: not yet implemented");
}

}  // namespace bdg::wish
