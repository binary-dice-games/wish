// MIT License © 2025 Binary Dice Games
/// @file mc.cpp
/// @brief Client-side runner for the mc embedded app.
///
/// The mc form (server-side) owns and renders both panels, but has
/// no direct access to the client's local machine. This runner is the
/// bridge: it reacts to the form's `on_local_navigate` event by enumerating
/// a local directory and reporting it back via `update_local_listing()`,
/// and to `on_upload_requested`/`on_download_requested` by moving bytes
/// between the local filesystem and the session sandbox.
#include "modules/bdg/desktop/mc/client/mc.hpp"

#include "src/client/app_registry.hpp"
#include "src/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
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
// race a concurrent session reader (e.g. an automation mc query) and spuriously
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
// update_local_listing(), the shape Mc::do_update_local_listing()
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

// Patches `transfer_progress`/`transfer_label` on the Mc form,
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

// Instantiates the built-in MessageBox form (see src/ui/forms/message_box.hpp)
// with a "yes_no" preset and calls @p on_yes if the user picks "Yes" --
// reuses the shared confirmation dialog instead of Mc building its
// own second modal window. Mirrors examples/demo/main.cpp's show_message_box:
// the MessageBox proxy is kept alive by capturing it in its own on_result
// handler, since a proxy with no live reference is destroyed immediately,
// taking the not-yet-answered dialog down with it.
void confirm_overwrite(wish_app_host& s, const std::string& message, std::function<void()> on_yes) {
  dynamic params;
  params["title"_key] = std::string{"Confirm Overwrite"};
  params["message"_key] = message;
  params["buttons"_key] = std::string{"yes_no"};
  auto raw = s.instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();
  auto mb = std::make_shared<rmi::proxy::dynamic>(std::move(raw));
  mb->onEvent("on_result"_key, [mb, on_yes = std::move(on_yes)](dynamic payload) {
    if (payload.as<std::string>("button"_key) == "yes")
      on_yes();
  });
}

} // namespace

void run_mc(wish_app_host& s) {
  auto explorer = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Mc"_key).get());
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

  // Reads `local_path/name`, pushes it into the sandbox, then asks the
  // server to re-list the sandbox panel. Shared by the no-conflict
  // ("on_upload_requested") and confirmed-overwrite ("on_upload_conflict" +
  // MessageBox "Yes") paths below.
  //
  // The actual transfer runs on a detached background thread rather than
  // inline in the caller: in standalone mode the handler itself runs on the
  // RMI dispatch thread while holding the session's write lock (see
  // bdg::bison::rmi::standalone's "do not block the worker from within an
  // event handler" contract), and that same lock is needed by the render
  // loop every frame. Blocking here for the whole transfer would freeze the
  // entire UI, not just the progress bar.
  auto do_upload = [&s](const std::shared_ptr<rmi::proxy::dynamic>& explorer, std::string name,
                        std::string local_path_str) {
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
  };

  // Pulls `name` from the sandbox and writes it into the currently-shown
  // local directory, then re-lists the left panel. Shared the same way as
  // do_upload above.
  auto do_download = [&s](const std::shared_ptr<rmi::proxy::dynamic>& explorer,
                          const std::shared_ptr<fs::path>& cur_dir, std::string name) {
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
  };

  // Upload button clicked with a local file selected and no name conflict.
  explorer->onEvent("on_upload_requested"_key, [explorer, do_upload](dynamic payload) {
    do_upload(explorer, payload.as<std::string>("name"_key), payload.as<std::string>("local_path"_key));
  });

  // Upload target already exists in the sandbox: confirm via a MessageBox
  // before overwriting (see confirm_overwrite() above).
  explorer->onEvent("on_upload_conflict"_key, [&s, explorer, do_upload](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    auto local_path_str = payload.as<std::string>("local_path"_key);
    confirm_overwrite(
        s, "\"" + name + "\" already exists in the sandbox. Overwrite it?",
        [explorer, do_upload, name, local_path_str]() { do_upload(explorer, name, local_path_str); });
  });

  // Download button clicked with a sandbox file selected and no name conflict.
  explorer->onEvent("on_download_requested"_key, [explorer, cur_dir, do_download](dynamic payload) {
    do_download(explorer, cur_dir, payload.as<std::string>("name"_key));
  });

  // Download target already exists locally: confirm via a MessageBox before
  // overwriting.
  explorer->onEvent("on_download_conflict"_key, [&s, explorer, cur_dir, do_download](dynamic payload) {
    auto name = payload.as<std::string>("name"_key);
    confirm_overwrite(
        s, "\"" + name + "\" already exists locally. Overwrite it?",
        [explorer, cur_dir, do_download, name]() { do_download(explorer, cur_dir, name); });
  });

  explorer->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });

  // Show the client's current working directory in the left panel at
  // startup, mirroring the sandbox panel's own root-on-open behavior.
  report_local_listing(explorer, cur_dir);

  // on_session() blocks until signal_done() is called.
}

namespace {
struct mc_app_registrar {
  mc_app_registrar() {
    register_app({
        .name = "mc",
        .organization = WISH_MODULE_BDG_DESKTOP_MC_ORGANIZATION,
        .collection = WISH_MODULE_BDG_DESKTOP_MC_COLLECTION,
        .description = "Two-panel file browser: local machine vs. session sandbox",
        .params = {},
        .run = run_mc,
    });
  }
};
const mc_app_registrar mc_app_registrar_instance;
} // namespace

} // namespace bdg::wish
