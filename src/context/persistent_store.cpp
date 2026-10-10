// MIT License © 2025 Binary Dice Games
/// @file persistent_store.cpp
/// @brief Implementation of wish::persistent_store and wish::store_registry.
#include <context/persistent_store.hpp>

#include <context/context.hpp>

#include "src/bison/bison_serialization.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bdg::wish {

using namespace bison;

namespace {

// File header: magic bytes followed by a one-byte format version. The body
// is one bison `dynamic` (standard binary format, see bison's FORMAT.md)
// holding an array of record objects {"name": string, "value": dynamic_ptr},
// one per entry. Records are index-keyed rather than keyed by
// key_t{name}: key_t is a 32-bit hash, so two names could collide and
// silently overwrite each other.
constexpr char kMagic[] = {'W', 'I', 'S', 'H', 'S', 'T', 'O', 'R', 'E'};
constexpr uint8_t kFormatVersion = 1;

void check_name(const std::string& name) {
  if (!persistent_store::is_valid_name(name))
    throw std::invalid_argument("wish::persistent_store: invalid entry name: \"" + name + "\"");
}

} // namespace

// ── persistent_store ─────────────────────────────────────────────────────────

persistent_store::persistent_store(std::filesystem::path file) : file_(std::move(file)) {
  auto entries = entries_.wlock();
  load(*entries);
}

bool persistent_store::is_valid_name(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameLength)
    return false;
  for (char c : name) {
    if (c < 0x20 || c > 0x7E)
      return false;
  }
  return true;
}

std::optional<dynamic> persistent_store::get(const std::string& name) const {
  check_name(name);
  auto entries = entries_.rlock();
  auto it = entries->find(name);
  if (it == entries->end())
    return std::nullopt;
  return it->second; // deep copy
}

void persistent_store::set(const std::string& name, const dynamic& value) {
  check_name(name);
  auto entries = entries_.wlock();
  std::optional<dynamic> previous;
  if (auto it = entries->find(name); it != entries->end())
    previous = std::move(it->second);
  entries->insert_or_assign(name, dynamic(value)); // deep copy
  try {
    save(*entries);
  } catch (...) {
    // Roll back so memory never diverges from what's on disk.
    if (previous)
      entries->insert_or_assign(name, std::move(*previous));
    else
      entries->erase(name);
    throw;
  }
}

bool persistent_store::erase(const std::string& name) {
  check_name(name);
  auto entries = entries_.wlock();
  auto node = entries->extract(name);
  if (node.empty())
    return false;
  try {
    save(*entries);
  } catch (...) {
    entries->insert(std::move(node));
    throw;
  }
  return true;
}

bool persistent_store::contains(const std::string& name) const {
  check_name(name);
  auto entries = entries_.rlock();
  return entries->find(name) != entries->end();
}

std::vector<std::string> persistent_store::keys() const {
  auto entries = entries_.rlock();
  std::vector<std::string> names;
  names.reserve(entries->size());
  for (const auto& [name, value] : *entries)
    names.push_back(name);
  return names;
}

void persistent_store::load(entry_map& entries) const {
  std::error_code ec;
  if (!std::filesystem::exists(file_, ec))
    return;

  try {
    std::ifstream in(file_, std::ios::binary);
    if (!in)
      throw std::runtime_error("cannot open file");
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (bytes.size() < sizeof(kMagic) + 1 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
      throw std::runtime_error("not a wish store file");
    if (static_cast<uint8_t>(bytes[sizeof(kMagic)]) != kFormatVersion)
      throw std::runtime_error("unsupported store format version");

    buffer_deserializer reader(bytes.data() + sizeof(kMagic) + 1, bytes.size() - sizeof(kMagic) - 1);
    const dynamic root = dynamic::deserialize(reader);
    entry_map loaded;
    root.forEach([&loaded](key_t, const field& f) {
      // Skip bison's own object metadata (e.g. the CLASS field), which is
      // never an object; every entry record is.
      if (!f.is<dynamic_ptr>())
        return;
      if (!f.as<dynamic_ptr>())
        throw std::runtime_error("malformed entry record");
      const auto& record = *f.as<dynamic_ptr>();
      const auto* name = record.findField("name"_key);
      const auto* value = record.findField("value"_key);
      if (!name || !name->is<std::string>() || !value || !value->is<dynamic_ptr>() || !value->as<dynamic_ptr>())
        throw std::runtime_error("malformed entry record");
      loaded.insert_or_assign(name->as<std::string>(), dynamic(*value->as<dynamic_ptr>()));
    });
    entries = std::move(loaded);
  } catch (const std::exception& e) {
    // Keep the unreadable file for inspection/recovery rather than letting
    // the next write overwrite it, and start empty.
    auto backup = file_;
    backup += ".corrupt";
    std::filesystem::rename(file_, backup, ec);
    std::cerr << "[wish] persistent store " << file_.string() << " is unreadable (" << e.what() << "); moved to "
              << backup.string() << " and starting empty\n";
    entries.clear();
  }
}

void persistent_store::save(const entry_map& entries) const {
  dynamic root;
  std::size_t idx = 0;
  for (const auto& [name, value] : entries) {
    auto record = dynamic_ptr{key_t{0U}, {}};
    (*record)["name"_key] = name;
    (*record)["value"_key] = dynamic_ptr{std::make_shared<dynamic>(value)};
    root[idx++] = record;
  }
  buffer_serializer writer;
  root.serialize(writer);
  const auto& body = writer.buffer();

  std::error_code ec;
  if (file_.has_parent_path())
    std::filesystem::create_directories(file_.parent_path(), ec);

  auto tmp = file_;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(kMagic, sizeof(kMagic));
    out.put(static_cast<char>(kFormatVersion));
    out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
    out.flush();
    if (!out)
      throw std::runtime_error("wish::persistent_store: cannot write " + tmp.string());
  }
  std::filesystem::rename(tmp, file_, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    throw std::runtime_error("wish::persistent_store: cannot replace " + file_.string());
  }
}

// ── default_store_dir ────────────────────────────────────────────────────────

std::filesystem::path default_store_dir() {
#if defined(_WIN32)
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  if (home && *home)
    return std::filesystem::path(home) / ".wish";
  return std::filesystem::path(".wish");
}

// ── store_registry ───────────────────────────────────────────────────────────

store_registry::store_registry(std::filesystem::path dir) : dir_(std::move(dir)) {}

persistent_store_ptr store_registry::server_store() {
  auto store = server_store_.wlock();
  if (!*store)
    *store = std::make_shared<persistent_store>(dir_ / "server_store.bison");
  return *store;
}

persistent_store_ptr store_registry::user_store(const std::string& identity) {
  if (!is_safe_sandbox_identity(identity))
    return nullptr;
  auto stores = user_stores_.wlock();
  auto& slot = (*stores)[identity];
  if (auto existing = slot.lock())
    return existing;
  auto store = std::make_shared<persistent_store>(dir_ / "users" / (identity + ".bison"));
  slot = store;
  return store;
}

} // namespace bdg::wish
