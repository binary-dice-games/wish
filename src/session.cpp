// MIT License © 2025 Binary Dice Games
/// @file session.cpp
/// @brief Implementation of the wish::session lifecycle.
#include <session.hpp>

#include "src/bison/bison_object.hpp"

#include <resource_store.hpp>

#include <algorithm>
#include <iomanip>
#include <system_error>
#include <vector>

namespace bdg::wish {

session::session(bison::key_t id_) : id(id_) {
  // Derive a directory name from the session id so that concurrent sessions
  // on the same machine do not collide.  Session IDs are unique per server
  // instance; the "wish_" prefix prevents clashing with other applications.
  resource_dir = std::filesystem::temp_directory_path() / ("wish_" + std::to_string(static_cast<uint32_t>(id.id)));
  std::filesystem::create_directories(resource_dir);
  // Return value intentionally ignored: a client that can't see built-in
  // icons/fonts is degraded, not fatal. extract_to() never throws, which
  // matters here -- this constructor runs on a per-connection worker thread
  // with no surrounding try/catch, so a thrown exception would terminate the
  // whole server, not just this connection.
  resource_store::extract_to(resource_dir / "res");
}

session::~session() {
  if (!resource_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(resource_dir, ec);
    // Ignore errors on cleanup (e.g. already removed externally).
  }
}

// ── Debug dump ────────────────────────────────────────────────────────────────

void dump_session_tree(const session& s, std::ostream& out) {
  using namespace bdg::bison;

  auto class_name = [](const ui_element& node) -> std::string {
    auto k = node.as<key_t>(dynamic::CLASS);
    if (const auto* proto = node.findClass(k)) {
      if (const auto* cls = proto->findField(dynamic::CLASS)) {
        if (const auto* dn = cls->findAttribute<DisplayName>())
          return dn->name();
      }
    }
    return "?(" + std::to_string(k.id) + ")";
  };

  // Collect and sort by key for stable, readable output.
  std::vector<std::pair<std::string, ui_element_ptr>> entries(s.objects.begin(), s.objects.end());
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

  out << "wish session objects (" << entries.size() << "):\n";
  for (const auto& [key, ptr] : entries) {
    if (!ptr) {
      out << "  [null]  \"" << key << "\"\n";
      continue;
    }
    out << "  [" << std::left << std::setw(20) << class_name(*ptr) << "]  \"" << key << "\"\n";
  }

  if (!s.top_level_objects.empty()) {
    out << "top_level_objects (" << s.top_level_objects.size() << "):\n";
    std::vector<std::pair<bison::key_t, ui_element_ptr>> roots(s.top_level_objects.begin(), s.top_level_objects.end());
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) {
      return static_cast<uint32_t>(a.first) < static_cast<uint32_t>(b.first);
    });
    for (const auto& [key, ptr] : roots) {
      if (!ptr) {
        out << "  [null]  [0x" << std::hex << static_cast<uint32_t>(key) << "]\n";
        continue;
      }
      out << "  [" << std::left << std::setw(20) << class_name(*ptr) << std::dec << "]  [0x" << std::hex
          << static_cast<uint32_t>(key) << "]\n";
    }
  }
}

} // namespace bdg::wish
