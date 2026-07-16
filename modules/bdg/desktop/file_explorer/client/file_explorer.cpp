// MIT License © 2025 Binary Dice Games
/// @file file_explorer.cpp
/// @brief Client-side runner for the FileExplorer embedded app.
///
/// The FileExplorer form (server-side) owns and renders both panels, but has
/// no direct access to the client's local machine. This runner is the
/// bridge: it reacts to the form's `on_local_navigate` event by enumerating
/// a local directory and reporting it back via `update_local_listing()`,
/// and to `on_upload_requested`/`on_download_requested` by moving bytes
/// between the local filesystem and the session sandbox.
#include "modules/bdg/desktop/file_explorer/client/file_explorer.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>{}};
}

void write_local_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string format_bytes(uintmax_t bytes) {
  static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < std::size(kUnits)) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << " " << kUnits[unit];
  return oss.str();
}

std::string format_modified(const fs::file_time_type& ftime) {
  auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &tt);
#else
  localtime_r(&tt, &tm_buf);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M");
  return oss.str();
}

// Invokes an RMI method on `explorer`, retrying a couple of times on
// failure. A method call placed immediately after a burst of chunked
// transfer RMI traffic (upload_chunk/download_chunk, each round-tripping
// through the same session dispatch) has been observed to occasionally
// race a concurrent session reader (e.g. an automation tree query) and spuriously
// fail with "Method not found" even though the method is registered --
// retrying after a short backoff reliably succeeds once that contention
// clears, without masking a *genuinely* missing method (which would keep
// failing every attempt and still surface after the retries are exhausted).
dynamic call_with_retry(
    const std::shared_ptr<rmi::proxy::dynamic>& explorer, key_t method, dynamic args, int attempts = 3) {
  for (int attempt = 1;; ++attempt) {
    try {
      return explorer->call(method, args.clone()).get();
    } catch (const std::exception&) {
      if (attempt >= attempts)
        throw;
      std::this_thread::sleep_for(std::chrono::milliseconds{50 * attempt});
    }
  }
}

// Enumerate `dir` and push the listing to the server via
// update_local_listing(), the shape FileExplorer::do_update_local_listing()
// expects: {path, files: [{name, type, size, modified}, ...]}.
void report_local_listing(
    const std::shared_ptr<rmi::proxy::dynamic>& explorer, const std::shared_ptr<fs::path>& cur_dir) {
  dynamic files;
  size_t i = 0;
  if (cur_dir->has_parent_path() && *cur_dir != cur_dir->root_path()) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{".."};
    (*e)["type"_key] = std::string{"dir"};
    (*e)["size"_key] = std::string{};
    (*e)["modified"_key] = std::string{};
    files[i++] = dynamic_ptr{e};
  }
  std::error_code ec;
  for (auto& dirent : fs::directory_iterator{*cur_dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    (*e)["type"_key] = is_dir ? std::string{"dir"} : std::string{"file"};
    (*e)["size"_key] = is_dir ? std::string{} : format_bytes(dirent.file_size(ec));
    auto ftime = dirent.last_write_time(ec);
    (*e)["modified"_key] = ec ? std::string{} : format_modified(ftime);
    files[i++] = dynamic_ptr{e};
  }

  dynamic args;
  args["path"_key] = cur_dir->string();
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  call_with_retry(explorer, "update_local_listing"_key, std::move(args));
}

// Patches `transfer_progress`/`transfer_label` on the FileExplorer form,
// throttled to whole-percent steps so a fast local transfer doesn't flood
// the dispatch queue with near-identical patches.
void report_transfer_progress(
    const std::shared_ptr<rmi::proxy::dynamic>& explorer, int& last_percent, std::uint64_t transferred,
    std::uint64_t total) {
  int percent = total == 0 ? 100 : static_cast<int>((transferred * 100) / total);
  if (percent == last_percent)
    return;
  last_percent = percent;

  dynamic patch;
  patch["transfer_progress"_key] = total == 0 ? 0.0f : static_cast<float>(transferred) / static_cast<float>(total);
  patch["transfer_label"_key] = format_bytes(transferred) + " / " + format_bytes(total);
  explorer->set(std::move(patch)).get();
}

// Clears the progress bar/label back to their idle state.
void clear_transfer_progress(const std::shared_ptr<rmi::proxy::dynamic>& explorer) {
  dynamic patch;
  patch["transfer_progress"_key] = 0.0f;
  patch["transfer_label"_key] = std::string{};
  explorer->set(std::move(patch)).get();
}

} // namespace

void run_file_explorer(wish_app_host& s) {
  auto explorer = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "FileExplorer"_key).get());
  auto cur_dir = std::make_shared<fs::path>(fs::current_path());

  // Server asks to browse a different local directory (row activated in the
  // left panel, or the local path bar's value was changed).
  explorer->onEvent("on_local_navigate"_key, [&s, explorer, cur_dir](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    auto type = payload.as<std::string>("type"_key);
    fs::path target = type == "path" ? fs::path(name) : (name == ".." ? cur_dir->parent_path() : (*cur_dir / name));
    std::error_code ec;
    if (!fs::is_directory(target, ec))
      return;
    *cur_dir = target;
    report_local_listing(explorer, cur_dir);
  });

  // Upload button clicked with a local file selected: read it, push it into
  // the sandbox, then ask the server to re-list the sandbox panel.
  //
  // The actual transfer runs on a detached background thread rather than
  // inline in this handler: in standalone mode the handler itself runs on
  // the RMI dispatch thread while holding the session's write lock (see
  // bdg::bison::rmi::standalone's "do not block the worker from within an
  // event handler" contract), and that same lock is needed by the render
  // loop every frame. Blocking here for the whole transfer would freeze the
  // entire UI, not just the progress bar.
  explorer->onEvent("on_upload_requested"_key, [&s, explorer](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    auto local_path_str = payload.as<std::string>("local_path"_key);
    fs::path local_path = fs::path(local_path_str) / name;

    std::thread([&s, explorer, name, local_path]() {
      try {
        auto data = read_local_file(local_path);
        int last_percent = -1;
        s.upload_file(
             name, data,
             [explorer, &last_percent](std::uint64_t transferred, std::uint64_t total) {
               report_transfer_progress(explorer, last_percent, transferred, total);
             })
            .get();
        clear_transfer_progress(explorer);
        call_with_retry(explorer, "refresh_sandbox"_key, dynamic{});
      } catch (const std::exception& e) {
        clear_transfer_progress(explorer);
        dynamic patch;
        patch["status"_key] = std::string{"Upload failed: "} + e.what();
        explorer->set(std::move(patch)).get();
      }
    }).detach();
  });

  // Download button clicked with a sandbox file selected: pull it, write it
  // into the currently-shown local directory, then re-list the left panel.
  // Same background-thread rationale as the upload handler above.
  explorer->onEvent("on_download_requested"_key, [&s, explorer, cur_dir](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);

    std::thread([&s, explorer, cur_dir, name]() {
      try {
        int last_percent = -1;
        auto data = s.download_file(
                         name,
                         [explorer, &last_percent](std::uint64_t transferred, std::uint64_t total) {
                           report_transfer_progress(explorer, last_percent, transferred, total);
                         })
                        .get();
        write_local_file(*cur_dir / name, data);
        clear_transfer_progress(explorer);
        report_local_listing(explorer, cur_dir);
      } catch (const std::exception& e) {
        clear_transfer_progress(explorer);
        dynamic patch;
        patch["status"_key] = std::string{"Download failed: "} + e.what();
        explorer->set(std::move(patch)).get();
      }
    }).detach();
  });

  explorer->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Show the client's current working directory in the left panel at
  // startup, mirroring the sandbox panel's own root-on-open behavior.
  report_local_listing(explorer, cur_dir);

  // on_session() blocks until signal_done() is called.
}

namespace {
struct file_explorer_app_registrar {
  file_explorer_app_registrar() {
    register_app({
        .name = "file_explorer",
        .organization = WISH_MODULE_BDG_DESKTOP_FILE_EXPLORER_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_FILE_EXPLORER_COLLECTION,
        .description = "Two-panel file browser: local machine vs. session sandbox",
        .params = {},
        .run = run_file_explorer,
    });
  }
};
const file_explorer_app_registrar file_explorer_app_registrar_instance;
} // namespace

} // namespace bdg::wish
