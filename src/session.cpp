// MIT License © 2025 Binary Dice Games
/// @file session.cpp
/// @brief Implementation of the wish::session lifecycle.
#include <wish/session.hpp>

#include <system_error>

namespace bdg::wish {

session::session(bison::key_t id_)
    : id(id_) {
  // Derive a directory name from the session id so that concurrent sessions
  // on the same machine do not collide.  Session IDs are unique per server
  // instance; the "wish_" prefix prevents clashing with other applications.
  resource_dir = std::filesystem::temp_directory_path() /
                 ("wish_" + std::to_string(static_cast<uint32_t>(id.id)));
  std::filesystem::create_directories(resource_dir);
}

session::~session() {
  if (!resource_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(resource_dir, ec);
    // Ignore errors on cleanup (e.g. already removed externally).
  }
}

session::session(session&& other) noexcept
    : id(other.id),
      objects(std::move(other.objects)),
      templates(std::move(other.templates)),
      resource_dir(std::move(other.resource_dir)),
      dirty(other.dirty.load()) {
  // Clear source so its destructor does not remove the transferred directory.
  other.resource_dir.clear();
}

session& session::operator=(session&& other) noexcept {
  if (this != &other) {
    // Clean up own resource_dir first.
    if (!resource_dir.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(resource_dir, ec);
    }
    id           = other.id;
    objects      = std::move(other.objects);
    templates    = std::move(other.templates);
    resource_dir = std::move(other.resource_dir);
    dirty.store(other.dirty.load());
    other.resource_dir.clear();
  }
  return *this;
}

}  // namespace bdg::wish
