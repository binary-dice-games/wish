// MIT License © 2025 Binary Dice Games
/// @file mc.cpp
/// @brief Client-side runner for the mc embedded app.
///
/// The mc form (server-side) owns and renders both panels, but has
/// no direct access to the client's local machine. This runner is the
/// bridge: it reacts to the form's `on_local_navigate` event by enumerating
/// a local directory and reporting it back via `update_local_listing()`,
/// to `on_local_tree_expand` by sending one more level of the folder tree
/// (`update_local_tree()`),
/// and to `on_upload_requested`/`on_download_requested` by moving bytes
/// between the local filesystem and the session sandbox.
///
/// Every one of those runs as a job on one `common::command_worker`
/// (modules/bdg/common/command_worker.hpp), never inside the event handler, so
/// the UI never freezes; a job that takes more than a moment opens the same
/// modal progress dialog the dev modules (docker, kubectl, ...) use -- for a
/// transfer, a determinate bar with bytes moved and a Cancel button.
#include "modules/bdg/desktop/mc/client/mc.hpp"

#include "modules/bdg/common/command_worker.hpp"

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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace bdg::wish {

using namespace bison;

namespace {

namespace fs = std::filesystem;

// Reads the plain-string array under `payload["names"]` -- see git.cpp's
// read_string_array() (modules/bdg/dev/git/server/git.cpp) for the same
// convention: array entries are raw std::string fields, not nested
// dynamic_ptr objects, since bison::field has no vector<string> alternative.
std::vector<std::string> read_names(const dynamic& payload) {
  std::vector<std::string> names;
  if (auto* names_f = payload.findField<dynamic_ptr>("names"_key); names_f && *names_f) {
    (*names_f)->forEach([&](key_t, const field& f) {
      if (f.is<std::string>())
        names.push_back(f.as<std::string>());
    });
  }
  return names;
}

// Reads the whole file in one sized read() -- a char-by-char
// istreambuf_iterator copy takes many seconds for a large file, all before
// the transfer reports any progress. Throws when the file can't be read, so
// the batch reports it instead of uploading an empty file.
std::string read_local_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());
  std::string data(static_cast<size_t>(in.tellg()), '\0');
  in.seekg(0);
  if (!in.read(data.data(), static_cast<std::streamsize>(data.size())))
    throw std::runtime_error("cannot read " + path.string());
  return data;
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
// expects: {path, files: [{name, type, size, modified}, ...], file_count,
// total_size, disk_used, disk_free, disk_total}. The last five are only
// computable client-side (this machine's own filesystem/disk), which is why
// mc's disk-usage summary strip is populated from here rather than
// server-side the way the sandbox panel's own strip is (see
// Mc::navigate_sandbox() in server/mc.cpp).
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
  int32_t file_count = 0;
  uintmax_t total_bytes = 0;
  std::error_code ec;
  for (auto& dirent : fs::directory_iterator{*cur_dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = dirent.path().filename().string();
    bool is_dir = dirent.is_directory(ec);
    (*e)["type"_key] = is_dir ? std::string{"dir"} : std::string{"file"};
    if (!is_dir) {
      uintmax_t bytes = dirent.file_size(ec);
      if (!ec) {
        ++file_count;
        total_bytes += bytes;
      }
      (*e)["size"_key] = format_bytes(bytes);
    } else {
      (*e)["size"_key] = std::string{};
    }
    auto ftime = dirent.last_write_time(ec);
    (*e)["modified"_key] = ec ? std::string{} : format_modified(ftime);
    files[i++] = dynamic_ptr{e};
  }

  dynamic args;
  args["path"_key] = cur_dir->string();
  args["files"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(files))};
  args["file_count"_key] = file_count;
  args["total_size"_key] = format_bytes(total_bytes);

  auto space_info = fs::space(*cur_dir, ec);
  if (!ec) {
    args["disk_used"_key] = format_bytes(space_info.capacity - space_info.free);
    args["disk_free"_key] = format_bytes(space_info.free);
    args["disk_total"_key] = format_bytes(space_info.capacity);
  }

  call_with_retry(explorer, "update_local_listing"_key, std::move(args));
}

// Top level of the Local Folders tree: the filesystem roots -- "/" on POSIX,
// every present drive ("C:\\", ...) on Windows.
std::vector<std::string> local_tree_roots() {
#if defined(_WIN32)
  std::vector<std::string> roots;
  for (char letter = 'A'; letter <= 'Z'; ++letter) {
    std::string root = std::string{letter} + ":\\";
    std::error_code ec;
    if (fs::is_directory(root, ec))
      roots.push_back(std::move(root));
  }
  return roots;
#else
  return {"/"};
#endif
}

// Sends one level of the Local Folders tree: the names of the directories
// directly under `path` (the roots themselves for an empty `path`), the shape
// Mc::do_update_local_tree() expects. An unreadable directory reports none.
void report_local_tree_level(const std::shared_ptr<rmi::proxy::dynamic>& explorer, const std::string& path) {
  std::vector<std::string> names;
  if (path.empty()) {
    names = local_tree_roots();
  } else {
    std::error_code ec;
    for (auto& dirent : fs::directory_iterator{fs::path(path), ec})
      if (dirent.is_directory(ec))
        names.push_back(dirent.path().filename().string());
  }

  dynamic dirs;
  size_t i = 0;
  for (auto& name : names)
    dirs[i++] = name;
  dynamic args;
  args["path"_key] = path;
  args["dirs"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(dirs))};
  call_with_retry(explorer, "update_local_tree"_key, std::move(args));
}

// Thrown from a transfer's progress callback once the user has pressed the
// progress dialog's Cancel, to abandon the chunked transfer mid-way.
struct transfer_cancelled : std::runtime_error {
  transfer_cancelled() : std::runtime_error("cancelled") {}
};

// "Uploading notes.txt" / "Downloading notes.txt (2 of 5)" -- the progress
// dialog's caption for one file of a batch.
std::string transfer_caption(const char* verb, const std::string& name, size_t index, size_t count) {
  std::string caption = std::string{verb} + " " + name;
  if (count > 1)
    caption += " (" + std::to_string(index + 1) + " of " + std::to_string(count) + ")";
  return caption;
}

// The progress callback shared by uploads and downloads: reports the bytes
// moved so far to the dialog, or abandons the transfer once cancelled. It
// runs on the transfer's own thread while the worker thread is blocked on
// that transfer's future, so the two never touch @p progress at once.
transfer_progress_callback transfer_reporter(common::task_progress& progress) {
  return [&progress](std::uint64_t transferred, std::uint64_t total) {
    if (progress.cancelled())
      throw transfer_cancelled{};
    progress.report(transferred, total, format_bytes(transferred) + " / " + format_bytes(total));
  };
}

// Builds the error a failed batch leaves in the progress dialog: one line per
// file that failed.
std::string batch_error(const char* verb, const std::vector<std::string>& failures) {
  std::string error = std::string{verb} + " failed:";
  for (auto& f : failures)
    error += "\n" + f;
  return error;
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
  params["icon"_key] = std::string{"question"};
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

  // Every handler below that touches the filesystem or moves bytes runs as a
  // job on this worker's thread, one at a time; long ones get the shared
  // modal progress dialog (modules/bdg/common/command_worker.hpp).
  auto worker = std::make_shared<common::command_worker>(s, "File Explorer");
  worker->start();

  // Server asks to browse a different local directory (row activated in the
  // Local panel, or the local path bar's value was changed).
  worker->on(*explorer, "on_local_navigate"_key, [explorer, cur_dir](const dynamic& payload) {
    auto name = payload.as<std::string>("name"_key);
    auto type = payload.as<std::string>("type"_key);
    fs::path target = type == "path" ? fs::path(name) : (name == ".." ? cur_dir->parent_path() : (*cur_dir / name));
    std::error_code ec;
    if (!fs::is_directory(target, ec))
      return;
    *cur_dir = target;
    report_local_listing(explorer, cur_dir);
  });

  // A Local Folders tree node was expanded (or is on the way to the
  // directory just navigated to) and its subdirectories are not known yet.
  worker->on(*explorer, "on_local_tree_expand"_key, [explorer](const dynamic& payload) {
    report_local_tree_level(explorer, payload.as<std::string>("path"_key));
  });

  // User confirmed the local panel's Rename dialog (server-side, since only
  // the client can touch its own filesystem -- see mc.hpp's class doc
  // comment).
  worker->on(*explorer, "on_local_rename_requested"_key, [explorer, cur_dir](const dynamic& payload) {
    auto old_name = payload.as<std::string>("old_name"_key);
    auto new_name = payload.as<std::string>("new_name"_key);
    std::error_code ec;
    fs::rename(*cur_dir / old_name, *cur_dir / new_name, ec);
    dynamic patch;
    patch["status"_key] = ec ? std::string{"Rename failed: "} + ec.message() : std::string{"Renamed."};
    explorer->set(std::move(patch)).get();
    report_local_listing(explorer, cur_dir);
  });

  // Uploads every entry of `names` from `local_path_str`, then asks the
  // server to re-list the Sandbox panel once the whole batch is done.
  // Shared by the no-conflict ("on_upload_requested") and
  // confirmed-overwrite ("on_upload_conflict" + MessageBox "Yes") paths
  // below. Runs as one worker job (never inside the event handler: in
  // standalone mode that runs on the RMI dispatch thread holding the
  // session's write lock, which the render loop needs every frame), one file
  // at a time so the progress dialog shows one coherent transfer. A failure
  // on one file is collected but doesn't abort the rest of the batch; Cancel
  // abandons the file in flight (its partial copy is discarded) and skips
  // the rest.
  auto do_upload = [&s, worker](const std::shared_ptr<rmi::proxy::dynamic>& explorer, std::vector<std::string> names,
                                std::string local_path_str, std::string sandbox_path_str) {
    worker->post([&s, worker, explorer, names = std::move(names), local_path_str, sandbox_path_str] {
      std::vector<std::string> failures;
      worker->run_task("Uploading", [&](common::task_progress& progress) {
        for (size_t i = 0; i < names.size() && !progress.cancelled(); ++i) {
          const std::string& name = names[i];
          const std::string remote_name = sandbox_path_str.empty() ? name : sandbox_path_str + "/" + name;
          progress.set_caption(transfer_caption("Uploading", name, i, names.size()));
          try {
            auto data = read_local_file(fs::path(local_path_str) / name);
            s.upload_file(remote_name, data, transfer_reporter(progress)).get();
          } catch (const transfer_cancelled&) {
            dynamic args;
            args["name"_key] = remote_name;
            call_with_retry(explorer, "discard_upload"_key, std::move(args));
          } catch (const std::exception& e) {
            failures.push_back(name + ": " + e.what());
          }
        }
      });
      if (!failures.empty())
        worker->fail(batch_error("Upload", failures), /*always_show=*/true);
      call_with_retry(explorer, "refresh_sandbox"_key, dynamic{});
    });
  };

  // Pulls every entry of `names` from the sandbox and writes it into the
  // currently-shown local directory, then re-lists the Local panel once.
  // Shared the same way as do_upload above; a cancelled download never
  // writes its file.
  auto do_download = [&s, worker](const std::shared_ptr<rmi::proxy::dynamic>& explorer,
                                  const std::shared_ptr<fs::path>& cur_dir, std::vector<std::string> names,
                                  std::string sandbox_path_str) {
    worker->post([&s, worker, explorer, cur_dir, names = std::move(names), sandbox_path_str] {
      std::vector<std::string> failures;
      worker->run_task("Downloading", [&](common::task_progress& progress) {
        for (size_t i = 0; i < names.size() && !progress.cancelled(); ++i) {
          const std::string& name = names[i];
          const std::string remote_name = sandbox_path_str.empty() ? name : sandbox_path_str + "/" + name;
          progress.set_caption(transfer_caption("Downloading", name, i, names.size()));
          try {
            auto data = s.download_file(remote_name, transfer_reporter(progress)).get();
            write_local_file(*cur_dir / name, data);
          } catch (const transfer_cancelled&) {
          } catch (const std::exception& e) {
            failures.push_back(name + ": " + e.what());
          }
        }
      });
      if (!failures.empty())
        worker->fail(batch_error("Download", failures), /*always_show=*/true);
      report_local_listing(explorer, cur_dir);
    });
  };

  // Upload button clicked with one or more local files selected and no name
  // conflicts.
  explorer->onEvent("on_upload_requested"_key, [explorer, do_upload](dynamic payload) {
    do_upload(explorer, read_names(payload), payload.as<std::string>("local_path"_key),
        payload.as<std::string>("sandbox_path"_key));
  });

  // Some/all of the upload targets already exist in the sandbox: confirm
  // once for the whole batch via a MessageBox before overwriting (see
  // confirm_overwrite() above).
  explorer->onEvent("on_upload_conflict"_key, [&s, explorer, do_upload](dynamic payload) {
    auto names = read_names(payload);
    auto local_path_str = payload.as<std::string>("local_path"_key);
    auto sandbox_path_str = payload.as<std::string>("sandbox_path"_key);
    std::string message = names.size() == 1
        ? "\"" + names[0] + "\" already exists in the sandbox. Overwrite it?"
        : std::to_string(names.size()) + " files already exist in the sandbox. Overwrite them?";
    confirm_overwrite(s, message, [explorer, do_upload, names, local_path_str, sandbox_path_str]() {
      do_upload(explorer, names, local_path_str, sandbox_path_str);
    });
  });

  // Download button clicked with one or more sandbox files selected and no
  // name conflicts.
  explorer->onEvent("on_download_requested"_key, [explorer, cur_dir, do_download](dynamic payload) {
    do_download(explorer, cur_dir, read_names(payload), payload.as<std::string>("sandbox_path"_key));
  });

  // Some/all of the download targets already exist locally: confirm once
  // for the whole batch via a MessageBox before overwriting.
  explorer->onEvent("on_download_conflict"_key, [&s, explorer, cur_dir, do_download](dynamic payload) {
    auto names = read_names(payload);
    auto sandbox_path_str = payload.as<std::string>("sandbox_path"_key);
    std::string message = names.size() == 1
        ? "\"" + names[0] + "\" already exists locally. Overwrite it?"
        : std::to_string(names.size()) + " files already exist locally. Overwrite them?";
    confirm_overwrite(s, message, [explorer, cur_dir, do_download, names, sandbox_path_str]() {
      do_download(explorer, cur_dir, names, sandbox_path_str);
    });
  });

  explorer->onEvent("closed"_key, [&s, worker](dynamic) {
    worker->shutdown();
    s.signal_done();
  });

  // Show the client's current working directory in the Local panel at
  // startup, mirroring the sandbox panel's own root-on-open behavior.
  // The folder tree gets only its top level; deeper levels are sent as their
  // nodes are expanded (on_local_tree_expand).
  worker->post([explorer, cur_dir] {
    report_local_tree_level(explorer, {});
    report_local_listing(explorer, cur_dir);
  });

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
