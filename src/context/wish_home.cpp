// MIT License © 2026 Binary Dice Games
/// @file wish_home.cpp
/// @brief `wish_home` and `normalize_identity()`, in the client library so
///        client-only programs (e.g. `wish desktop`) can locate `~/.wish`
///        too.
#include <context/wish_home.hpp>

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace bdg::wish {

namespace {

constexpr size_t kMaxIdentityLength = 64;

std::filesystem::path env_path(const char* name) {
  const char* v = std::getenv(name);
  if (v && *v)
    return std::filesystem::path(v);
  return {};
}

} // namespace

std::string normalize_identity(const std::string& identity) {
  if (identity.empty() || identity.size() > kMaxIdentityLength || identity.front() == '.')
    return {};
  std::string out;
  out.reserve(identity.size());
  for (char c : identity) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
    else if (c == ' ')
      c = '_';
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok)
      return {};
    out.push_back(c);
  }
  if (out.find("..") != std::string::npos)
    return {};
  return out;
}

wish_home::wish_home() : root_(default_root()) {}

wish_home::wish_home(std::filesystem::path root) : root_(root.empty() ? default_root() : std::move(root)) {}

std::filesystem::path wish_home::default_root() {
  if (auto p = env_path("WISH_HOME"); !p.empty())
    return p;
#if defined(_WIN32)
  auto home = env_path("USERPROFILE");
#else
  auto home = env_path("HOME");
#endif
  if (!home.empty())
    return home / ".wish";
  return std::filesystem::path(".wish");
}

std::filesystem::path wish_home::server_store_file() const {
  return root_ / "server" / "store.bison";
}

std::filesystem::path wish_home::users_dir() const {
  return root_ / "users";
}

std::filesystem::path wish_home::user_dir(const std::string& identity) const {
  const std::string id = normalize_identity(identity);
  if (id.empty())
    return {};
  return users_dir() / id;
}

std::filesystem::path wish_home::user_store_file(const std::string& identity) const {
  auto dir = user_dir(identity);
  return dir.empty() ? dir : dir / "store.bison";
}

std::filesystem::path wish_home::user_sandbox_dir(const std::string& identity) const {
  auto dir = user_dir(identity);
  return dir.empty() ? dir : dir / "sandbox";
}

std::filesystem::path wish_home::user_desktop_config(const std::string& identity) const {
  auto dir = user_dir(identity);
  return dir.empty() ? dir : dir / "desktop" / "desktop.yaml";
}

std::filesystem::path wish_home::logs_dir() const {
  return root_ / "logs";
}

std::filesystem::path wish_home::screenshots_dir() const {
  return root_ / "screenshots";
}

std::filesystem::path wish_home::traces_dir() const {
  return root_ / "traces";
}

std::vector<std::string> wish_home::list_identities() const {
  std::vector<std::string> ids;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(users_dir(), ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec))
      continue;
    const std::string name = it->path().filename().string();
    // Only names already in normalized form: anything else was not created
    // by wish and would map to a different directory.
    if (normalize_identity(name) == name)
      ids.push_back(name);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace bdg::wish
