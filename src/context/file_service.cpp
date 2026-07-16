// MIT License (c) 2025 Binary Dice Games
/// @file file_service.cpp
/// @brief Implementation of the wish file service.
#include <context/file_service.hpp>

#include <net/http_client.hpp>

#include "src/bison/bison_sync.hpp"

#include <miniz.h>
#include <miniz_zip.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace bdg::wish {

using namespace bdg::bison;

namespace {

std::string lowercase(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Extension (without the leading '.'), lowercased, extracted from a URL's
// path component -- ignoring the scheme/host and any query string/fragment.
// Empty if the URL has no path or no extension.
std::string url_extension(const std::string& url) {
  auto scheme_end = url.find("://");
  std::string rest = scheme_end == std::string::npos ? url : url.substr(scheme_end + 3);
  auto path_start = rest.find('/');
  if (path_start == std::string::npos)
    return {};
  std::string path = rest.substr(path_start);

  auto query = path.find_first_of("?#");
  if (query != std::string::npos)
    path.resize(query);

  auto slash = path.find_last_of('/');
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return {};
  return lowercase(path.substr(dot + 1));
}

std::filesystem::path
url_cache_path(const std::filesystem::path& resource_dir, const std::string& url, const std::string& ext) {
  // A hash of the full URL is used as the cache key -- this is purely a
  // cache filename, not a security boundary, so collision resistance beyond
  // std::hash is unnecessary.
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016zx", std::hash<std::string>{}(url));
  return resource_dir / "url_cache" / (std::string(buf) + "." + ext);
}

// Cache paths for URLs whose download is currently in flight -- prevents the
// render loop (which calls resolve_or_fetch every frame) from starting a
// second background download for the same URL while the first is running.
bison::synchronized<std::unordered_set<std::filesystem::path>>& pending_downloads() {
  static bison::synchronized<std::unordered_set<std::filesystem::path>> pending;
  return pending;
}

// Cache paths for URLs whose download has already failed once -- prevents
// retrying a permanently-broken URL on every subsequent frame.
bison::synchronized<std::unordered_set<std::filesystem::path>>& failed_downloads() {
  static bison::synchronized<std::unordered_set<std::filesystem::path>> failed;
  return failed;
}

} // namespace

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

std::filesystem::path file_service::resolve_or_fetch(
    const std::string& name,
    const std::filesystem::path& resource_dir,
    bool allow_absolute,
    bool allow_fetch,
    const std::vector<std::string>& allowed_extensions) {
  if (name.starts_with("http://") || name.starts_with("https://")) {
    if (!allow_fetch)
      return {};

    auto ext = url_extension(name);
    bool allowed = !ext.empty() &&
        std::any_of(allowed_extensions.begin(), allowed_extensions.end(),
            [&](const std::string& e) { return lowercase(e) == ext; });
    if (!allowed)
      return {};

    auto cache_path = url_cache_path(resource_dir, name, ext);

    std::error_code exists_ec;
    if (std::filesystem::exists(cache_path, exists_ec))
      return cache_path;

    if (failed_downloads().rlock()->contains(cache_path))
      return {};

    // Atomically claim this URL's download slot: only the frame that
    // actually inserts a new entry starts the background thread.
    bool claimed = pending_downloads().wlock()->insert(cache_path).second;
    if (claimed) {
      std::thread([name, cache_path] {
        std::error_code dir_ec;
        std::filesystem::create_directories(cache_path.parent_path(), dir_ec);

        bool success = false;
        if (!dir_ec) {
          auto response = net::http_get(name);
          if (response.ok) {
            auto staging = cache_path;
            staging += ".part";
            std::ofstream out(staging, std::ios::binary);
            if (out) {
              out.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
              out.close();
              std::error_code rename_ec;
              std::filesystem::rename(staging, cache_path, rename_ec);
              success = !rename_ec;
            }
          }
        }

        if (!success)
          failed_downloads().wlock()->insert(cache_path);
        pending_downloads().wlock()->erase(cache_path);
      }).detach();
    }
    return {};
  }

  if (name.starts_with("file://")) {
    std::filesystem::path p{name.substr(7)};
    if (!p.is_absolute())
      return {};
    return allow_absolute ? p : std::filesystem::path{};
  }

  return resolve_path(name, resource_dir, allow_absolute);
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
