// MIT License © 2025 Binary Dice Games
/// @file file_service.cpp
/// @brief Implementation of the wish file service.
#include <wish/file_service.hpp>

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace bdg::wish {

using namespace bdg::bison;

// ── file_service_node ─────────────────────────────────────────────────────────

file_service_node::file_service_node(dynamic&& base,
                                     std::filesystem::path resource_dir)
    : dynamic(std::move(base)),
      resource_dir_(std::move(resource_dir)) {
  addMethod(
      "upload"_key,
      bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        upload(p.as<std::string>("name"_key), p.as<std::string>("data"_key));
        return dynamic{};
      }});
  addMethod(
      "download"_key,
      bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        dynamic result;
        result["result"_key] = download(p.as<std::string>("name"_key));
        return result;
      }});
  addMethod(
      "erase"_key,
      bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
        erase(p.as<std::string>("name"_key));
        return dynamic{};
      }});
}

std::filesystem::path
file_service_node::resolve_path(const std::string& name) const {
  if (name.empty()) {
    throw std::runtime_error(
        "wish::file_service: path must not be empty");
  }
  // Normalize both paths purely syntactically — resolves '..' and '.' without
  // touching the filesystem, so it works for files that do not yet exist.
  auto base   = resource_dir_.lexically_normal();
  auto target = (resource_dir_ / name).lexically_normal();
  auto rel    = target.lexically_relative(base);

  // If the relative path starts with ".." the target is outside the sandbox.
  if (rel.empty() || *rel.begin() == "..") {
    throw std::runtime_error(
        "wish::file_service: path \"" + name +
        "\" escapes the resource directory");
  }
  return target;
}

void file_service_node::upload(const std::string& name,
                               const std::string& data) {
  auto path = resolve_path(name);
  if (!std::filesystem::exists(resource_dir_)) {
    throw std::runtime_error(
        "wish::file_service: resource directory does not exist");
  }
  // Create any intermediate subdirectories within the sandbox.
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error(
        "wish::file_service: cannot write \"" + name + "\"");
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string file_service_node::download(const std::string& name) const {
  auto path = resolve_path(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error(
        "wish::file_service: file not found: \"" + name + "\"");
  }
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>{}};
}

bison::dynamic_ptr file_service_node::list() const {
  auto result = dynamic_ptr{key_t{0U}, {}};
  std::size_t idx = 0;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(resource_dir_, ec)) {
    if (entry.is_regular_file()) {
      (*result)[idx++] = entry.path().filename().string();
    }
  }
  return result;
}

void file_service_node::erase(const std::string& name) {
  auto path = resolve_path(name);
  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    throw std::runtime_error(
        "wish::file_service: cannot delete \"" + name + "\"");
  }
}

// ── register_file_service ────────────────────────────────────────────────────

void register_file_service() {
  auto proto = dynamic_ptr{"__WishFS"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

}  // namespace bdg::wish
