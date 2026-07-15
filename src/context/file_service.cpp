// MIT License (c) 2025 Binary Dice Games
/// @file file_service.cpp
/// @brief Implementation of the wish file service.
#include <context/file_service.hpp>

#include <miniz.h>
#include <miniz_zip.h>

#include <fstream>
#include <stdexcept>
#include <system_error>

namespace bdg::wish {

using namespace bdg::bison;

// ── file_service ──────────────────────────────────────────────────────────────

file_service_ptr file_service::instantiate(std::filesystem::path resource_dir) {
    return std::make_shared<file_service>(
        bison::dynamic::instantiate(bison::key_t{"wish"}, bison::key_t{"__WishFileSystem"}), std::move(resource_dir));
}

file_service::file_service(dynamic&& base, std::filesystem::path resource_dir)
    : dynamic(std::move(base)), resource_dir_(std::move(resource_dir)) {
  addMethod("upload"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              upload(p.as<std::string>("name"_key), p.as<std::string>("data"_key));
              return dynamic{};
            }});
  addMethod("download"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              dynamic result;
              result["result"_key] = download(p.as<std::string>("name"_key));
              return result;
            }});
  addMethod("list"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              std::string path;
              if (const auto* f = p.findField("path"_key); f && f->is<std::string>())
                path = f->as<std::string>();
              return *list(path);
            }});
  addMethod("erase"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              erase(p.as<std::string>("name"_key));
              return dynamic{};
            }});
  addMethod("upload_chunk"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              upload_chunk(
                  p.as<std::string>("name"_key),
                  p.as<std::string>("data"_key),
                  p.as<bool>("first"_key),
                  p.as<bool>("eof"_key));
              return dynamic{};
            }});
  addMethod("download_chunk"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              auto c = download_chunk(
                  p.as<std::string>("name"_key), p.as<int32_t>("offset"_key), p.as<int32_t>("max_size"_key));
              dynamic result;
              result["data"_key] = std::move(c.data);
              result["eof"_key] = c.eof;
              result["total"_key] = static_cast<int32_t>(c.total);
              return result;
            }});
  addMethod("unpack"_key, bison::method{[this](dynamic& /*self*/, const dynamic& p) -> dynamic {
              unpack(p.as<std::string>("zip_name"_key), p.as<std::string>("dest"_key));
              return dynamic{};
            }});
}

std::filesystem::path
file_service::resolve_path(const std::string& name, const std::filesystem::path& resource_dir, bool allow_absolute) {
  if (name.empty())
    return {};

  std::filesystem::path p{name};
  if (p.is_absolute())
    return allow_absolute ? p : std::filesystem::path{};

  // Normalize purely syntactically -- resolves ".." and "." without touching
  // the filesystem, so it works for files that do not yet exist.
  auto base = resource_dir.lexically_normal();
  auto target = (resource_dir / name).lexically_normal();
  auto rel = target.lexically_relative(base);

  // A relative path that starts with ".." escapes the sandbox.
  if (rel.empty() || *rel.begin() == std::filesystem::path(".."))
    return {};

  return target;
}

std::filesystem::path file_service::resolve_path(const std::string& name) const {
  auto result = resolve_path(name, resource_dir_, /*allow_absolute=*/false);
  if (result.empty()) {
    if (name.empty())
      throw std::runtime_error("wish::file_service: path must not be empty");
    throw std::runtime_error("wish::file_service: path escapes the resource directory: " + name);
  }
  return result;
}

void file_service::upload(const std::string& name, const std::string& data) {
  auto path = resolve_path(name);
  if (!std::filesystem::exists(resource_dir_)) {
    throw std::runtime_error("wish::file_service: resource directory does not exist");
  }
  // Create any intermediate subdirectories within the sandbox.
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw std::runtime_error("wish::file_service: cannot write: " + name);
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string file_service::download(const std::string& name) const {
  auto path = resolve_path(name);
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("wish::file_service: file not found: " + name);
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

void file_service::upload_chunk(const std::string& name, const std::string& data, bool first, bool eof) {
  auto path = resolve_path(name);
  if (!std::filesystem::exists(resource_dir_)) {
    throw std::runtime_error("wish::file_service: resource directory does not exist");
  }
  // Create any intermediate subdirectories within the sandbox.
  std::filesystem::create_directories(path.parent_path());

  // Chunks accumulate in a staging file so a transfer interrupted mid-stream
  // never leaves a corrupt file at the user-visible name -- only an orphaned
  // ".wishpart" file.
  std::filesystem::path staging = path;
  staging += ".wishpart";
  {
    std::ofstream out(staging, std::ios::binary | (first ? std::ios::trunc : std::ios::app));
    if (!out) {
      throw std::runtime_error("wish::file_service: cannot write: " + name);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  if (eof) {
    std::error_code ec;
    std::filesystem::rename(staging, path, ec);
    if (ec) {
      throw std::runtime_error("wish::file_service: cannot finalize upload: " + name + ": " + ec.message());
    }
  }
}

file_service::chunk file_service::download_chunk(const std::string& name, int32_t offset, int32_t max_size) const {
  auto path = resolve_path(name);
  if (offset < 0 || max_size < 0) {
    throw std::runtime_error("wish::file_service: invalid offset/max_size: " + name);
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("wish::file_service: file not found: " + name);
  }

  in.seekg(0, std::ios::end);
  auto total = static_cast<std::uint64_t>(in.tellg());
  in.seekg(offset, std::ios::beg);
  std::string data(static_cast<std::size_t>(max_size), '\0');
  in.read(data.data(), max_size);
  data.resize(static_cast<std::size_t>(in.gcount()));

  // A read that fills the whole request doesn't set eofbit until the stream
  // actually tries to read past the end -- peek() forces that check so a
  // chunk landing exactly on EOF is still reported correctly.
  bool eof = in.eof() || in.peek() == std::char_traits<char>::eof();
  return chunk{std::move(data), eof, total};
}

void file_service::unpack(const std::string& zip_name, const std::string& dest) {
  auto zip_path = resolve_path(zip_name);
  auto dest_path = resolve_path(dest);

  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, zip_path.string().c_str(), 0)) {
    throw std::runtime_error("wish::file_service: cannot open archive: " + zip_name);
  }

  std::error_code ec;
  std::filesystem::create_directories(dest_path, ec);
  if (ec) {
    mz_zip_reader_end(&zip);
    throw std::runtime_error("wish::file_service: cannot create destination: " + dest + ": " + ec.message());
  }

  mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    if (mz_zip_reader_is_file_a_directory(&zip, i))
      continue;

    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("wish::file_service: corrupt archive entry in: " + zip_name);
    }

    // Zip-slip guard: unlike the build-controlled embedded resource archive,
    // this zip's entries are client-supplied content and must be treated as
    // untrusted -- reuse the same lexical escape check upload()/download()
    // use, rooted at dest_path instead of resource_dir_.
    auto target = resolve_path(std::string{st.m_filename}, dest_path, /*allow_absolute=*/false);
    if (target.empty()) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("wish::file_service: archive entry escapes destination: " + std::string{st.m_filename});
    }

    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec || !mz_zip_reader_extract_to_file(&zip, i, target.string().c_str(), 0)) {
      mz_zip_reader_end(&zip);
      throw std::runtime_error("wish::file_service: failed to extract: " + std::string{st.m_filename});
    }
  }

  mz_zip_reader_end(&zip);
  std::filesystem::remove(zip_path, ec);
}

bison::dynamic_ptr file_service::list(const std::string& path) const {
  // Empty path lists the resource directory's own root; resolve_path()
  // throws on an empty name, so that case bypasses it entirely rather than
  // being treated as an escape attempt.
  auto dir = path.empty() ? resource_dir_ : resolve_path(path);

  auto result = dynamic_ptr{key_t{0U}, {}};
  std::size_t idx = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file()) {
      (*result)[idx++] = entry.path().filename().string();
    }
  }
  return result;
}

void file_service::erase(const std::string& name) {
  auto path = resolve_path(name);
  std::error_code ec;
  if (!std::filesystem::remove(path, ec)) {
    throw std::runtime_error("wish::file_service: cannot delete: " + name);
  }
}

// ── register_file_service ──────────────────────────────────────────────────────

void register_file_service() {
  auto proto = dynamic_ptr{"__WishFileSystem"_key, {}};
  dynamic::addClass("wish"_key, std::move(proto));
}

} // namespace bdg::wish
