// MIT License © 2025 Binary Dice Games
/// @file context.cpp
/// @brief Implementation of the wish::context lifecycle.
#include <context/context.hpp>

#include "src/bison/bison_common.hpp"
#include "src/bison/bison_object.hpp"
#include "src/rmi/shared/constants.hpp"

#include <algorithm>
#include <iomanip>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <context/file_service.hpp>
#include <context/logger.hpp>
#include <resource_store.hpp>
#include <context/style_service.hpp>

#ifdef WISH_AUTOMATION_ENABLED
#include <automation/automation_service.hpp>
#endif

#include <ui/forms/form.hpp>

#include "ui/ui_elements/object_inspector.hpp"
#include "ui/ui_template.hpp"

namespace bdg::wish {

using namespace bison;

namespace detail {

bison::dynamic_ptr find_singleton_service(const context& s, bison::key_t klass) {
  if (klass == "__WishFileSystem"_key && s.file_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.file_service)};
  if (klass == "__WishStyle"_key && s.style_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.style_service)};
  if (klass == "__WishLogger"_key && s.logger_service)
    return dynamic_ptr{std::static_pointer_cast<dynamic>(s.logger_service)};
#ifdef WISH_AUTOMATION_ENABLED
  if (klass == "__WishAutomation"_key) {
    // Unlike the singletons above, this one is conditionally present: only
    // set when the active renderer implements automation::automation_backend
    // (see context::automation_service's doc comment). Throwing here (rather
    // than falling through to the "not a singleton class" sentinel below,
    // which would let bison's default on_create_object construct a useless
    // object from register_automation_service()'s method-less prototype)
    // turns "this server doesn't support automation" into a clear exception
    // at instantiate() time -- wish::client::on_connect() catches it
    // non-fatally and leaves automation_proxy_ unset.
    if (s.automation_service)
      return dynamic_ptr{std::static_pointer_cast<dynamic>(s.automation_service)};
    throw std::runtime_error("wish: this server's active renderer does not support automation");
  }
#endif
  // NOTE: dynamic_ptr{} is NOT a null pointer -- dynamic_ptr's
  // (key_t klass = 0U, ...) constructor shadows shared_ptr's null default,
  // so it would build a real (but methodless) object here instead.  Use the
  // base shared_ptr's null constructor explicitly to signal "no match".
  return dynamic_ptr{std::shared_ptr<dynamic>{}};
}

void init_session_object(const bison::dynamic_ptr& obj, bison::rmi::context& ctx, const sync_context_ptr& sync_ctx) {
  if (!obj || !sync_ctx)
    return;
  if (auto* h = dynamic_cast<ui_template*>(obj.get())) {
    h->init(ctx, sync_ctx);
  } else if (auto* f = dynamic_cast<form*>(obj.get())) {
    f->init(ctx, sync_ctx);
  } else if (auto* oi = dynamic_cast<object_inspector*>(obj.get())) {
    oi->init(ctx, sync_ctx);
  }
}

bool is_read_only_op(bison::key_t op) {
  namespace k = bison::rmi::shared::constants;
  return op == k::OP_GET || op == k::OP_DESCRIBE || op == k::OP_DICTIONARY || op == k::OP_HELP;
}

} // namespace detail

context::context(bison::key_t id) : bison::rmi::context(id) {
  // Derive a directory name from the session id so that concurrent sessions
  // on the same machine do not collide.  Session IDs are unique per server
  // instance; the "wish_" prefix prevents clashing with other applications.
  resource_dir = std::filesystem::temp_directory_path() / ("wish_" + std::to_string(static_cast<uint32_t>(id.id)));
  // populate_resource_dir() never throws, which matters here -- this
  // constructor runs on a per-connection worker thread with no surrounding
  // try/catch, so a thrown exception would terminate the whole server, not
  // just this connection.
  populate_resource_dir();
}

context::~context() {
  if (!resource_dir.empty() && !resource_dir_persistent) {
    std::error_code ec;
    std::filesystem::remove_all(resource_dir, ec);
    // Ignore errors on cleanup (e.g. already removed externally).
  }
}

void context::populate_resource_dir() {
  std::filesystem::create_directories(resource_dir);
  // Return value intentionally ignored: a client that can't see built-in
  // icons/fonts is degraded, not fatal. extract_to() never throws.
  std::unordered_map<std::string, uint32_t> raw_crc32;
  resource_store::extract_to(resource_dir / "res", &raw_crc32);
  // Re-key with a "res/" prefix so a lookup by embedded_crc32s[src] matches
  // the resource_dir-relative src an Image element resolves to (see
  // imgui_ui_renderer.cpp's render_image / web_renderer::get_or_load_texture).
  for (auto& [rel, crc] : raw_crc32)
    embedded_crc32s["res/" + rel] = crc;
}

// ── Debug dump ────────────────────────────────────────────────────────────────

void dump_session_tree(const context& s, std::ostream& out) {
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
  std::vector<std::pair<std::string, ui_element_ptr>> entries(s.ui_objects.begin(), s.ui_objects.end());
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
