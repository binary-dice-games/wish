// MIT License © 2025 Binary Dice Games
/// @file notepad.cpp
/// @brief Client-side runner for the Notepad embedded app.
///
/// The Notepad form (server-side) never touches the client's local
/// filesystem -- it only edits files already sitting in its session
/// sandbox. This runner is the bridge: it reacts to the form's high-level
/// events by driving the client's own local files through `upload_file` /
/// `download_file`.
///
/// A file to open at startup may be passed after `--` on the command line,
/// e.g. `wish client --run=notepad -- path/to/file` (see
/// `wish_app_host::app_args()`).
#include "modules/notepad/client/notepad.hpp"

#include "app/wish_cli/client/apps/app_registry.hpp"
#include "app/wish_cli/client/wish_app_host.hpp"

#include "src/bison/bison.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>

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

// Build the `files` dynamic expected by FileDialog from a directory listing.
// Mirrors list_directory() in examples/demo/main.cpp.
dynamic list_directory(const fs::path& dir) {
  dynamic files;
  size_t i = 0;
  if (dir.has_parent_path() && dir != dir.root_path()) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{".."};
    (*e)["type"_key] = std::string{"dir"};
    files[i++] = dynamic_ptr{e};
  }
  std::error_code ec;
  for (auto& entry : fs::directory_iterator{dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = entry.path().filename().string();
    (*e)["type"_key] = entry.is_directory(ec) ? std::string{"dir"} : std::string{"file"};
    files[i++] = dynamic_ptr{e};
  }
  return files;
}

/// Tracks the sandbox name <-> local path mapping for files this client has
/// uploaded, and picks a sandbox name that does not collide with one
/// already in use (e.g. two different directories each containing a
/// "notes.txt"). Re-opening the exact same local path twice is not
/// deduplicated here -- the server already no-ops a duplicate open_file
/// call for a given sandbox path, so at worst this produces two
/// independently-edited tabs backed by two sandbox copies of one file.
struct sandbox_files {
  std::map<std::string, std::string> local_path_by_sandbox_name;

  std::string reserve_name(const fs::path& local_path) {
    auto candidate = local_path.filename().string();
    int suffix = 1;
    while (local_path_by_sandbox_name.count(candidate))
      candidate = local_path.stem().string() + "_" + std::to_string(suffix++) + local_path.extension().string();
    return candidate;
  }
};

// Upload a local file's current contents into the sandbox under a fresh
// name and register it as a new Notepad tab. Shared by the FileDialog-driven
// Open/New flows and by opening a file passed on the command line.
void upload_and_open(
    wish_app_host& s,
    const std::shared_ptr<rmi::proxy::dynamic>& notepad,
    const std::shared_ptr<sandbox_files>& files,
    const fs::path& local_path) {
  auto data = read_local_file(local_path);
  auto sandbox_name = files->reserve_name(local_path);
  s.upload_file(sandbox_name, data).get();
  files->local_path_by_sandbox_name[sandbox_name] = local_path.string();

  dynamic args;
  args["path"_key] = sandbox_name;
  args["title"_key] = local_path.filename().string();
  notepad->call("open_file"_key, std::move(args)).get();
}

// Shared by "Open" and "New": show a FileDialog populated from a local
// directory listing (same pattern as examples/demo/main.cpp's browse()
// helper), then upload the chosen file and register it via open_file().
// `create_if_missing` is set for "New", where the chosen path need not
// already exist locally.
void browse_and_open(
    wish_app_host& s,
    std::shared_ptr<rmi::proxy::dynamic> notepad,
    std::shared_ptr<sandbox_files> files,
    std::string title,
    std::string confirm_label,
    bool create_if_missing) {
  auto cur_dir = std::make_shared<fs::path>(fs::current_path());
  auto dlg = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "FileDialog"_key).get());

  dynamic init;
  init["title"_key] = title;
  init["confirm_label"_key] = confirm_label;
  init["path"_key] = cur_dir->string();
  init["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory(*cur_dir))};
  dlg->set(std::move(init)).get();

  dlg->onEvent("on_navigate"_key, [dlg, cur_dir](dynamic payload) mutable {
    auto name = payload.as<std::string>("name"_key);
    auto type = payload.as<std::string>("type"_key);
    if (type == "path")
      *cur_dir = fs::path(name);
    else
      *cur_dir = (name == "..") ? cur_dir->parent_path() : (*cur_dir / name);
    dynamic next;
    next["path"_key] = cur_dir->string();
    next["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory(*cur_dir))};
    dlg->set(std::move(next));
  });

  dlg->onEvent("on_open"_key, [&s, notepad, files, cur_dir, create_if_missing](dynamic payload) {
    auto name = payload.as<std::string>("path"_key);
    fs::path local_path = fs::path(name).is_absolute() ? fs::path(name) : (*cur_dir / name);

    // "New": the user typed/picked a path that may not exist yet -- create it
    // empty. If it already exists (e.g. they picked an existing file by
    // mistake), leave its content alone rather than truncating it.
    if (create_if_missing && !fs::exists(local_path))
      write_local_file(local_path, "");

    upload_and_open(s, notepad, files, local_path);
  });

  // on_cancel: the dialog already removed itself from session.objects;
  // the capture just keeps dlg alive until one of its events fires.
  dlg->onEvent("on_cancel"_key, [dlg](dynamic) {});
}

} // namespace

void run_notepad(wish_app_host& s) {
  auto notepad = std::make_shared<rmi::proxy::dynamic>(s.instantiate("wish"_key, "Notepad"_key).get());
  auto files = std::make_shared<sandbox_files>();

  // "Open" clicked: the server has no view of the client's local files, so
  // it asks us to present our own picker.
  notepad->onEvent("on_request_open"_key, [&s, notepad, files](dynamic) {
    browse_and_open(s, notepad, files, "Open File", "Open", /*create_if_missing=*/false);
  });

  // "New" clicked: same handshake, but the chosen path need not exist yet.
  notepad->onEvent("on_request_new"_key, [&s, notepad, files](dynamic) {
    browse_and_open(s, notepad, files, "New File", "New", /*create_if_missing=*/true);
  });

  // A tab (or the whole window) closed: download this file one last time
  // and forget our local bookkeeping for it.
  notepad->onEvent("on_file_closed"_key, [&s, files](dynamic payload) {
    auto path = payload.as<std::string>("path"_key);
    auto it = files->local_path_by_sandbox_name.find(path);
    if (it == files->local_path_by_sandbox_name.end())
      return;
    write_local_file(it->second, s.download_file(path).get());
    files->local_path_by_sandbox_name.erase(it);
  });

  // Ctrl+S inside a tab: download that one file, keep it open.
  notepad->onEvent("on_file_saved"_key, [&s, files](dynamic payload) {
    auto path = payload.as<std::string>("path"_key);
    auto it = files->local_path_by_sandbox_name.find(path);
    if (it != files->local_path_by_sandbox_name.end())
      write_local_file(it->second, s.download_file(path).get());
  });

  // "Sync" clicked: force-download every currently open file.
  notepad->onEvent("on_sync_requested"_key, [&s, files](dynamic payload) {
    auto* paths_f = payload.findField<dynamic_ptr>("paths"_key);
    if (!paths_f || !*paths_f)
      return;
    (*paths_f)->forEach([&](key_t, const field& f) {
      if (!f.is<std::string>())
        return;
      auto path = f.as<std::string>();
      auto it = files->local_path_by_sandbox_name.find(path);
      if (it != files->local_path_by_sandbox_name.end())
        write_local_file(it->second, s.download_file(path).get());
    });
  });

  notepad->onEvent("closed"_key, [&s](dynamic) { s.signal_done(); });
  // Nothing else to keep alive: `notepad`'s shared_ptr lives on inside the
  // lambdas above (registered as this session's event handlers), which is
  // enough to keep the proxy usable for the whole session.

  // `wish client --run=notepad -- path/to/file`: open a file at startup.
  // Only the first positional argument is used; extras are ignored.
  if (const auto& app_args = s.app_args(); !app_args.empty()) {
    fs::path local_path = fs::absolute(app_args[0]);
    if (fs::exists(local_path))
      upload_and_open(s, notepad, files, local_path);
    else
      std::cerr << "[notepad] no such file: " << local_path.string() << '\n';
  }

  // on_session() blocks until signal_done() is called.
}

namespace {
struct notepad_app_registrar {
  notepad_app_registrar() {
    register_app({
        .name = "notepad",
        .description = "Multi-file, syntax-highlighted text editor",
        .params = {{"file", "Path to a file to open at startup (optional)"}},
        .run = run_notepad,
    });
  }
};
const notepad_app_registrar notepad_app_registrar_instance;
} // namespace

} // namespace bdg::wish
